#!/usr/bin/env python3
"""Build reproducible DVE shader compilation evidence when external toolchains are available.

The script never treats source-contract inspection as compiled shader evidence. Missing optional
compilers are recorded explicitly and only become fatal when --require-backend names that backend.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_string(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def run_command(command: list[str], cwd: Path) -> dict[str, Any]:
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    return {
        "command": command,
        "command_text": command_string(command),
        "return_code": completed.returncode,
        "duration_ms": (time.perf_counter() - started) * 1000.0,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def stage_for_glslang(stage: str) -> str:
    return {"compute": "comp", "vertex": "vert", "pixel": "frag"}[stage]


def inspect_tools() -> dict[str, str | None]:
    return {
        "python": sys.executable,
        "dxc": shutil.which("dxc"),
        "glslangValidator": shutil.which("glslangValidator"),
        "spirv-cross": shutil.which("spirv-cross"),
        "spirv-val": shutil.which("spirv-val"),
        "spirv-dis": shutil.which("spirv-dis"),
        "xcrun": shutil.which("xcrun"),
    }


def compile_vulkan(record: dict[str, Any], source: Path, output: Path,
                   shader_root: Path, tools: dict[str, str | None]) -> dict[str, Any]:
    output.parent.mkdir(parents=True, exist_ok=True)
    dxc = tools["dxc"]
    glslang = tools["glslangValidator"]
    if dxc:
        command = [dxc, "-spirv", "-fspv-target-env=vulkan1.1", "-T", record["profile"],
                   "-E", record["entry"], "-I", str(shader_root), "-Fo", str(output), str(source)]
        compiler = "dxc"
    elif glslang:
        command = [glslang, "-D", "-V", "--target-env", "vulkan1.1", "-S",
                   stage_for_glslang(record["stage"]), "-e", record["entry"],
                   f"-I{shader_root}", "-o", str(output), str(source)]
        compiler = "glslangValidator"
    else:
        return {"status": "unavailable", "reason": "dxc and glslangValidator were not found"}
    result = run_command(command, shader_root.parent)
    result["compiler"] = compiler
    if result["return_code"] != 0 or not output.is_file():
        result["status"] = "failed"
        return result
    result["status"] = "compiled"
    result["output"] = output.relative_to(output.parents[2]).as_posix()
    result["bytes"] = output.stat().st_size
    result["sha256"] = sha256_file(output)
    validator = tools["spirv-val"]
    if validator:
        validation = run_command([validator, "--target-env", "vulkan1.1", str(output)], shader_root.parent)
        result["spirv_validation"] = validation
        if validation["return_code"] != 0:
            result["status"] = "failed"
    spirv_cross = tools["spirv-cross"]
    if spirv_cross:
        reflected = run_command([spirv_cross, str(output), "--reflect"], shader_root.parent)
        result["reflection_command"] = reflected
        if reflected["return_code"] == 0:
            reflection_path = output.with_suffix(".reflection.json")
            reflection_path.write_text(reflected["stdout"], encoding="utf-8")
            verification_path = output.with_suffix(".reflection_check.json")
            verification = run_command([
                sys.executable,
                str(shader_root.parent / "tools/verify_compiled_shader_reflection.py"),
                "--manifest", str(shader_root / "shader_manifest.json"),
                "--shader", record["name"],
                "--reflection", str(reflection_path),
                "--output", str(verification_path),
            ], shader_root.parent)
            result["reflection"] = {
                **verification,
                "status": "passed" if verification["return_code"] == 0 else "failed",
                "reflection_output": str(reflection_path),
                "verification_output": str(verification_path),
            }
            if verification["return_code"] != 0:
                result["status"] = "failed"
        else:
            result["reflection"] = {"status": "failed", "reason": "spirv-cross reflection failed"}
            result["status"] = "failed"
    else:
        result["reflection"] = {"status": "unavailable", "reason": "spirv-cross was not found"}
    return result


def compile_d3d12(record: dict[str, Any], source: Path, output: Path,
                  shader_root: Path, tools: dict[str, str | None]) -> dict[str, Any]:
    dxc = tools["dxc"]
    if not dxc:
        return {"status": "unavailable", "reason": "dxc was not found"}
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [dxc, "-T", record["profile"], "-E", record["entry"],
               "-I", str(shader_root), "-Fo", str(output), str(source)]
    result = run_command(command, shader_root.parent)
    result["compiler"] = "dxc"
    if result["return_code"] != 0 or not output.is_file():
        result["status"] = "failed"
        return result
    result["status"] = "compiled"
    result["output"] = output.relative_to(output.parents[2]).as_posix()
    result["bytes"] = output.stat().st_size
    result["sha256"] = sha256_file(output)
    return result


def compile_metal(record: dict[str, Any], source: Path, output: Path,
                  shader_root: Path, tools: dict[str, str | None], intermediate: Path) -> dict[str, Any]:
    dxc = tools["dxc"]
    spirv_cross = tools["spirv-cross"]
    if not dxc or not spirv_cross:
        return {"status": "unavailable", "reason": "dxc and spirv-cross are required for HLSL-to-MSL evidence"}
    intermediate.parent.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    first = run_command([dxc, "-spirv", "-fspv-target-env=vulkan1.1", "-T", record["profile"],
                         "-E", record["entry"], "-I", str(shader_root), "-Fo",
                         str(intermediate), str(source)], shader_root.parent)
    if first["return_code"] != 0 or not intermediate.is_file():
        first["status"] = "failed"
        first["phase"] = "hlsl_to_spirv"
        return first
    second = run_command([spirv_cross, str(intermediate), "--msl", "--output", str(output)], shader_root.parent)
    result: dict[str, Any] = {"compiler": "dxc+spirv-cross", "phases": [first, second]}
    if second["return_code"] != 0 or not output.is_file():
        result["status"] = "failed"
        return result
    result["status"] = "translated"
    result["output"] = output.relative_to(output.parents[2]).as_posix()
    result["bytes"] = output.stat().st_size
    result["sha256"] = sha256_file(output)
    xcrun = tools["xcrun"]
    if xcrun and sys.platform == "darwin":
        air = output.with_suffix(".air")
        metal_compile = run_command([xcrun, "-sdk", "macosx", "metal", "-c", str(output), "-o", str(air)], shader_root.parent)
        result["metal_compile"] = metal_compile
        if metal_compile["return_code"] == 0 and air.is_file():
            result["air_sha256"] = sha256_file(air)
        else:
            result["status"] = "failed"
    else:
        result["metal_compile"] = {"status": "unavailable", "reason": "xcrun metal is only available on macOS"}
    return result


def summary_markdown(report: dict[str, Any]) -> str:
    lines = ["# DVE Shader Compilation Evidence", "",
             f"- Source contract: **{report['source_contract']['status']}**",
             f"- Overall: **{report['status']}**", "", "## Backends", "",
             "| Backend | Compiled/translated | Failed | Unavailable |", "|---|---:|---:|---:|"]
    for backend, totals in report["totals"].items():
        lines.append(f"| {backend} | {totals['compiled']} | {totals['failed']} | {totals['unavailable']} |")
    lines += ["", "## Tool inventory", ""]
    for name, path in report["tools"].items():
        lines.append(f"- `{name}`: `{path or 'not found'}`")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", action="append", choices=["vulkan", "d3d12", "metal"])
    parser.add_argument("--require-backend", action="append", choices=["vulkan", "d3d12", "metal"], default=[])
    parser.add_argument("--require-reflection", action="store_true",
                        help="Fail unless every compiled Vulkan shader passes SPIRV-Cross reflection parity")
    args = parser.parse_args()
    root = args.root.resolve()
    output_root = args.output.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    shader_root = root / "shaders"
    manifest = json.loads((shader_root / "shader_manifest.json").read_text(encoding="utf-8"))
    backends = args.backend or ["vulkan", "d3d12", "metal"]
    tools = inspect_tools()

    source_validation = run_command([sys.executable, str(root / "tools/validate_shader_contracts.py"),
                                     "--root", str(root)], root)
    source_contract = {
        **source_validation,
        "status": "passed" if source_validation["return_code"] == 0 else "failed",
    }
    records: list[dict[str, Any]] = []
    totals = {backend: {"compiled": 0, "failed": 0, "unavailable": 0} for backend in backends}
    for shader in manifest.get("shaders", []):
        source = shader_root / shader["source"]
        record: dict[str, Any] = {
            "name": shader["name"], "source": shader["source"], "stage": shader["stage"],
            "entry": shader["entry"], "profile": shader["profile"],
            "source_sha256": sha256_file(source), "bindings": shader.get("bindings", []),
            "threads": shader.get("threads"), "backends": {},
        }
        for backend in backends:
            if backend == "vulkan":
                result = compile_vulkan(shader, source, output_root / "compiled/vulkan" / f"{shader['name']}.spv", shader_root, tools)
            elif backend == "d3d12":
                result = compile_d3d12(shader, source, output_root / "compiled/d3d12" / f"{shader['name']}.dxil", shader_root, tools)
            else:
                result = compile_metal(shader, source, output_root / "compiled/metal" / f"{shader['name']}.metal",
                                       shader_root, tools, output_root / "compiled/metal_spirv" / f"{shader['name']}.spv")
            record["backends"][backend] = result
            if result["status"] in ("compiled", "translated"):
                totals[backend]["compiled"] += 1
            elif result["status"] == "failed":
                totals[backend]["failed"] += 1
            else:
                totals[backend]["unavailable"] += 1
        records.append(record)

    failed = source_contract["status"] == "failed" or any(t["failed"] for t in totals.values())
    missing_required = any(totals[backend]["unavailable"] for backend in args.require_backend)
    reflection_missing = args.require_reflection and any(
        shader.get("backends", {}).get("vulkan", {}).get("reflection", {}).get("status") != "passed"
        for shader in records
    )
    report = {
        "schema": 1,
        "status": "failed" if failed or missing_required or reflection_missing else "passed",
        "root": str(root),
        "manifest_sha256": sha256_file(shader_root / "shader_manifest.json"),
        "tools": tools,
        "source_contract": source_contract,
        "totals": totals,
        "shaders": records,
        "limitations": [
            "Source inspection is not compiled shader evidence.",
            "Metal translation is not Metal compiler evidence unless xcrun metal succeeds.",
            "Successful compilation and reflection are not physical-device execution evidence.",
        ],
    }
    (output_root / "shader_compilation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (output_root / "summary.md").write_text(summary_markdown(report), encoding="utf-8")
    print(f"DVE shader evidence: {report['status']} -> {output_root}")
    return 1 if report["status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
