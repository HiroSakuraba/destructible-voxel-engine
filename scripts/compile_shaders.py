#!/usr/bin/env python3
"""Build DVE's canonical HLSL into backend artifacts and a binding contract.

DXIL and SPIR-V require Microsoft's DirectX Shader Compiler (`dxc`). Metal source additionally
requires `spirv-cross`; native metallib packaging requires Apple's `xcrun` on macOS. `--dry-run`
validates the manifest and prints exact commands without requiring those tools.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Shader:
    name: str
    source: Path
    entry: str
    profile: str
    threads: tuple[int, int, int] | None
    bindings: tuple[dict[str, str], ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("shaders/shader_manifest.json"))
    parser.add_argument("--out", type=Path, default=Path("out/shaders"))
    parser.add_argument("--backend", choices=("all", "dxil", "spirv", "metal"), default="all")
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--spirv-cross", dest="spirv_cross", type=Path)
    parser.add_argument("--xcrun", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def executable(explicit: Path | None, name: str) -> str | None:
    if explicit is not None:
        return str(explicit)
    return shutil.which(name)


def load_manifest(path: Path) -> tuple[dict, list[Shader]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != 1:
        raise ValueError("unsupported shader manifest schema")
    root = path.parent
    shaders: list[Shader] = []
    names: set[str] = set()
    for item in payload.get("shaders", []):
        name = item["name"]
        if name in names:
            raise ValueError(f"duplicate shader name: {name}")
        names.add(name)
        stage = item.get("stage", "compute")
        raw_threads = item.get("threads")
        threads = tuple(int(value) for value in raw_threads) if raw_threads is not None else None
        if stage == "compute" and (threads is None or len(threads) != 3 or any(value <= 0 for value in threads)):
            raise ValueError(f"invalid thread group for {name}")
        if stage != "compute" and threads is not None:
            raise ValueError(f"graphics shader {name} must not declare threads")
        shaders.append(
            Shader(
                name=name,
                source=root / item["source"],
                entry=item["entry"],
                profile=item["profile"],
                threads=(threads[0], threads[1], threads[2]) if threads is not None else None,
                bindings=tuple(item.get("bindings", [])),
            )
        )
    if not shaders:
        raise ValueError("shader manifest contains no shaders")
    return payload, shaders



def expanded_source(path: Path, root: Path, visited: set[Path] | None = None) -> str:
    visited = set() if visited is None else visited
    resolved = path.resolve()
    if resolved in visited:
        return ""
    if root.resolve() not in resolved.parents and resolved != root.resolve():
        raise ValueError(f"shader include escapes root: {path}")
    visited.add(resolved)
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(r'^\s*#include\s+"([^"]+)"', re.M)
    chunks: list[str] = []
    cursor = 0
    for match in pattern.finditer(text):
        chunks.append(text[cursor:match.start()])
        chunks.append(expanded_source(path.parent / match.group(1), root, visited))
        cursor = match.end()
    chunks.append(text[cursor:])
    return "\n".join(chunks)

def validate_source(shader: Shader) -> None:
    text = expanded_source(shader.source, shader.source.parent)
    entry_pattern = rf"\b\w+(?:<[^>]+>)?\s+{re.escape(shader.entry)}\s*\("
    if re.search(entry_pattern, text) is None:
        raise ValueError(f"{shader.name}: entry point {shader.entry!r} not found")
    if shader.threads is not None:
        thread_pattern = rf"\[numthreads\(\s*{shader.threads[0]}\s*,\s*{shader.threads[1]}\s*,\s*{shader.threads[2]}\s*\)\]"
        if re.search(thread_pattern, text) is None:
            raise ValueError(f"{shader.name}: numthreads differs from manifest")
    for binding in shader.bindings:
        register = binding["register"]
        if f"register({register})" not in text:
            raise ValueError(f"{shader.name}: binding {binding['name']} lacks register({register})")


def run(command: list[str], dry_run: bool) -> None:
    print(" ".join(command))
    if not dry_run:
        subprocess.run(command, check=True)


def requested_backends(value: str) -> tuple[str, ...]:
    return ("dxil", "spirv", "metal") if value == "all" else (value,)


def build_shader(shader: Shader, out: Path, backends: Iterable[str], args: argparse.Namespace) -> dict:
    requested = tuple(backends)
    dxc = executable(args.dxc, "dxc")
    spirv_cross = executable(args.spirv_cross, "spirv-cross")
    xcrun = executable(args.xcrun, "xcrun")
    outputs: dict[str, str] = {}
    shader_out = out / shader.name
    shader_out.mkdir(parents=True, exist_ok=True)

    if requested and dxc is None and not args.dry_run:
        raise RuntimeError("dxc was not found; pass --dxc or use --dry-run")
    dxc_command = dxc or "dxc"

    if "dxil" in requested:
        output = shader_out / f"{shader.name}.dxil"
        run([dxc_command, "-T", shader.profile, "-E", shader.entry, "-Fo", str(output), str(shader.source)], args.dry_run)
        outputs["dxil"] = str(output)

    spirv: Path | None = None
    if "spirv" in requested or "metal" in requested:
        spirv = shader_out / f"{shader.name}.spv"
        run([
            dxc_command, "-spirv", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout",
            "-T", shader.profile, "-E", shader.entry, "-Fo", str(spirv), str(shader.source)
        ], args.dry_run)
        outputs["spirv"] = str(spirv)

    if "metal" in requested:
        assert spirv is not None
        if spirv_cross is None and not args.dry_run:
            raise RuntimeError("spirv-cross was not found; pass --spirv-cross or use --dry-run")
        metal = shader_out / f"{shader.name}.metal"
        run([spirv_cross or "spirv-cross", str(spirv), "--msl", "--output", str(metal)], args.dry_run)
        outputs["metal_source"] = str(metal)
        if xcrun is not None or args.dry_run:
            air = shader_out / f"{shader.name}.air"
            metallib = shader_out / f"{shader.name}.metallib"
            run([xcrun or "xcrun", "-sdk", "macosx", "metal", "-c", str(metal), "-o", str(air)], args.dry_run)
            run([xcrun or "xcrun", "-sdk", "macosx", "metallib", str(air), "-o", str(metallib)], args.dry_run)
            outputs["metallib"] = str(metallib)
    return outputs


def main() -> int:
    args = parse_args()
    try:
        manifest, shaders = load_manifest(args.manifest)
        for shader in shaders:
            validate_source(shader)
        args.out.mkdir(parents=True, exist_ok=True)
        backends = requested_backends(args.backend)
        compiled = []
        for shader in shaders:
            compiled.append({
                "name": shader.name,
                "entry": shader.entry,
                "profile": shader.profile,
                "threads": list(shader.threads) if shader.threads is not None else None,
                "bindings": list(shader.bindings),
                "outputs": build_shader(shader, args.out, backends, args),
            })
        contract = {
            "schema": 1,
            "canonical_language": manifest["canonical_language"],
            "requested_backends": list(backends),
            "shaders": compiled,
        }
        contract_path = args.out / "shader_contract.json"
        contract_path.write_text(json.dumps(contract, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {contract_path}")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"shader build failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
