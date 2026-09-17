#!/usr/bin/env python3
"""Configure, build, and run DVE's external simulation/device validation package."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path


def run(command: list[str], cwd: Path, environment: dict[str, str] | None = None) -> None:
    print("+", " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, env=environment, check=False)
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def write_root_summary(output: Path, invocation: dict, shader: dict, device: dict) -> None:
    adapter = device.get("device", {}) if isinstance(device.get("device"), dict) else {}
    warnings = device.get("warnings", []) if isinstance(device.get("warnings"), list) else []
    totals = shader.get("totals", {}) if isinstance(shader.get("totals"), dict) else {}
    lines = [
        "# DVE External Device Validation Evidence",
        "",
        f"- Device result: **{device.get('status', 'unavailable')}**",
        f"- Shader evidence: **{shader.get('status', 'unavailable')}**",
        f"- Backend: `{invocation.get('backend', '')}`",
        f"- Adapter: `{adapter.get('adapter_name', 'unavailable')}`",
        f"- Adapter class: `{adapter.get('adapter_class', 'unknown')}`",
        f"- Execution class: `{adapter.get('execution_class', 'unknown')}`",
        f"- Physical adapter confirmed: `{str(adapter.get('physical_adapter_confirmed', False)).lower()}`",
        f"- Physical adapter required: `{str(invocation.get('require_physical', False)).lower()}`",
        f"- Kernel readback required: `{str(invocation.get('require_kernel_readback', False)).lower()}`",
        f"- Production-pass specialization readback required: `{str(invocation.get('require_production_readback', False)).lower()}`",
        f"- Timestamp queries required: `{str(invocation.get('require_timestamps', False)).lower()}`",
        f"- Device runner exit code: `{invocation.get('device_exit_code', -1)}`",
        "",
        "## Shader backends",
        "",
        "| Backend | Compiled/translated | Failed | Unavailable |",
        "|---|---:|---:|---:|",
    ]
    for backend in ("vulkan", "d3d12", "metal"):
        row = totals.get(backend, {}) if isinstance(totals.get(backend), dict) else {}
        lines.append(
            f"| {backend} | {row.get('compiled', 0)} | {row.get('failed', 0)} | "
            f"{row.get('unavailable', 0)} |")
    lines.extend(["", "## Warnings", ""])
    if warnings:
        lines.extend(f"- {warning}" for warning in warnings)
    else:
        lines.append("- None recorded.")
    lines.extend([
        "",
        "Successful shader compilation is not device execution evidence. Software or virtual Vulkan "
        "execution is not physical-GPU evidence.",
        "",
    ])
    (output / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def write_file_manifest(output: Path) -> None:
    records = []
    for path in sorted(output.rglob("*")):
        if path.is_file() and path.name != "file_manifest.json":
            records.append({
                "path": path.relative_to(output).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            })
    payload = {"schema": 1, "files": records}
    (output / "file_manifest.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", choices=["null", "vulkan"], default="vulkan")
    parser.add_argument("--suite", choices=["device", "simulation", "all"], default="all")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--require-physical", action="store_true")
    parser.add_argument("--require-kernel-readback", action="store_true")
    parser.add_argument("--require-production-readback", action="store_true")
    parser.add_argument("--require-timestamps", action="store_true")
    parser.add_argument("--require-shader-backend", action="append",
                        choices=["vulkan", "d3d12", "metal"], default=[])
    parser.add_argument("--vk-icd", type=Path)
    args = parser.parse_args()

    root = args.source_root.resolve()
    build = (args.build_dir or root / "build-device-validation").resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    started = time.time()

    if not args.skip_build:
        run(["cmake", "-S", str(root), "-B", str(build), "-G", args.generator,
             f"-DCMAKE_BUILD_TYPE={args.config}", "-DDVE_BUILD_DEVICE_TEST_RUNNER=ON",
             "-DDVE_BUILD_RHI=ON", "-DDVE_ENABLE_VULKAN_BUFFER_BACKEND=ON",
             "-DDVE_BUILD_EDITOR=OFF", "-DDVE_BUILD_NATIVE_EDITOR=OFF",
             "-DDVE_BUILD_AUDIO_SYNTH=OFF", "-DDVE_BUILD_ASSET_COOKER=OFF",
             "-DDVE_BUILD_ASSET_PIPELINE_TESTS=OFF", "-DDVE_BUILD_BENCHMARK=OFF",
             "-DDVE_ENABLE_SDL3_HOST=OFF", "-DDVE_BUILD_LEGACY_X11_HOST=OFF"], root)
        run(["cmake", "--build", str(build), "--config", args.config,
             "--target", "dve_device_test_runner"], root)

    executable_name = "dve_device_test_runner.exe" if os.name == "nt" else "dve_device_test_runner"
    runner = args.runner.resolve() if args.runner else build / executable_name
    if not runner.is_file() and args.config:
        candidate = build / args.config / executable_name
        if candidate.is_file():
            runner = candidate
    if not runner.is_file():
        raise FileNotFoundError(f"device runner was not found: {runner}")

    shader_output = output / "shader_evidence"
    shader_command = [sys.executable, str(root / "tools/build_shader_evidence.py"),
                      "--root", str(root), "--output", str(shader_output)]
    for backend in args.require_shader_backend:
        shader_command.extend(["--require-backend", backend])
    shader_result = subprocess.run(shader_command, cwd=root, check=False)

    command = [str(runner), "--backend", args.backend, "--suite", args.suite,
               "--output", str(output / "device_evidence"), "--source-root", str(root),
               "--external-execution"]
    if args.require_physical:
        command.append("--require-physical")
    if args.require_kernel_readback:
        command.append("--require-kernel-readback")
    if args.require_production_readback:
        command.append("--require-production-readback")
    if args.require_timestamps:
        command.append("--require-timestamps")
    environment = os.environ.copy()
    if args.vk_icd:
        environment["VK_ICD_FILENAMES"] = str(args.vk_icd.resolve())
    device_result = subprocess.run(command, cwd=root, env=environment, check=False)

    invocation = {
        "schema": 1,
        "source_root": str(root),
        "build_dir": str(build),
        "runner": str(runner),
        "backend": args.backend,
        "suite": args.suite,
        "require_physical": args.require_physical,
        "require_kernel_readback": args.require_kernel_readback,
        "require_production_readback": args.require_production_readback,
        "require_timestamps": args.require_timestamps,
        "required_shader_backends": args.require_shader_backend,
        "vk_icd": environment.get("VK_ICD_FILENAMES", ""),
        "shader_exit_code": shader_result.returncode,
        "device_exit_code": device_result.returncode,
        "elapsed_seconds": time.time() - started,
        "python": sys.version,
        "platform": sys.platform,
    }
    (output / "invocation.json").write_text(json.dumps(invocation, indent=2) + "\n", encoding="utf-8")
    shader_report = read_json(shader_output / "shader_compilation.json")
    device_report = read_json(output / "device_evidence/run.json")
    write_root_summary(output, invocation, shader_report, device_report)
    write_file_manifest(output)

    archive = output.with_suffix(".zip")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in sorted(output.rglob("*")):
            if path.is_file():
                bundle.write(path, path.relative_to(output.parent))
    print(f"Evidence archive: {archive}")
    if shader_result.returncode != 0:
        return shader_result.returncode
    return device_result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
