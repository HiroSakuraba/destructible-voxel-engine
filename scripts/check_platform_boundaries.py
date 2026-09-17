#!/usr/bin/env python3
"""Fail when portable targets directly include native window-system or graphics-API headers."""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

FORBIDDEN = re.compile(
    r"#\s*include\s*[<\"](?:SDL3/|X11/|windows\.h|d3d12\.h|dxgi\w*\.h|vulkan/|Metal/|Cocoa/|AppKit/)",
    re.IGNORECASE,
)


def portable_files(root: pathlib.Path) -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    include_root = root / "include" / "dve"
    for path in include_root.rglob("*.hpp"):
        relative = path.relative_to(include_root)
        if relative.parts[0] in {"platform", "rhi", "gpu"}:
            continue
        if relative.name == "editor_sdl_canvas.hpp":
            continue
        files.append(path)

    src_root = root / "src"
    for path in src_root.glob("*.cpp"):
        if path.name == "editor_sdl_canvas.cpp":
            continue
        files.append(path)
    files.append(src_root / "render" / "brickmap_rhi_mirror.cpp")
    return sorted(path for path in files if path.exists())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    violations: list[str] = []
    scanned = portable_files(root)
    for path in scanned:
        for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if FORBIDDEN.search(line):
                violations.append(f"{path.relative_to(root)}:{line_number}: {line.strip()}")
    print(f"portable files scanned: {len(scanned)}")
    if violations:
        print("platform-boundary violations:")
        print("\n".join(violations))
        return 1
    print("platform-boundary check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
