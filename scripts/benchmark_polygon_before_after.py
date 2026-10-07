#!/usr/bin/env python3
"""Compare a historical renderer using the current benchmark object and build flags.

Requires a Linux Ninja Release build with compile_commands.json and the
dve_polygon_render_bench target already built. Does not modify the source tree.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--baseline", default="5569fef")
parser.add_argument("--ninja", default="ninja")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
database = json.loads((build / "compile_commands.json").read_text())
entry = next(e for e in database if e["file"].endswith("/src/render/polygon_renderer.cpp"))
with tempfile.TemporaryDirectory(prefix="polygon_benchmark_") as temporary:
    folder = Path(temporary)
    source, obj, binary = (folder / name for name in ("before.cpp", "before.o", "before"))
    source.write_bytes(subprocess.check_output(
        ["git", "show", f"{args.baseline}:src/render/polygon_renderer.cpp"], cwd=root))
    command = shlex.split(entry["command"])
    command[command.index("-o") + 1] = str(obj)
    command[command.index("-c") + 1] = str(source)
    subprocess.run(command, cwd=entry["directory"], check=True)
    link = subprocess.check_output(
        [args.ninja, "-C", str(build), "-t", "commands", "dve_polygon_render_bench"], text=True).splitlines()[-1]
    command = shlex.split(link.removeprefix(": && ").removesuffix(" && :"))
    command[command.index("-o") + 1] = str(binary)
    command[command.index("libdve_render_bridge.a")] = str(obj)
    command = [item for item in command if not item.startswith("-Wl,--dependency-file=")]
    subprocess.run(command, cwd=build, check=True)
    before, after = [json.loads(subprocess.check_output([str(executable)], text=True))
                     for executable in (binary, build / "dve_polygon_render_bench")]
    if before["frame_hash"] != after["frame_hash"]:
        raise SystemExit("framebuffer mismatch")
    # The historical source predates instrumentation: zero counters are unknown.
    for key in list(before):
        if key not in {"instances", "texture", "median_ms", "frame_hash"}:
            del before[key]
    print(json.dumps({"baseline": args.baseline, "before": before, "after": after,
                      "speedup": before["median_ms"] / after["median_ms"]}, indent=2))
