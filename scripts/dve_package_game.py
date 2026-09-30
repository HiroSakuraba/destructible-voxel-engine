#!/usr/bin/env python3
"""dve_package_game: build a shippable game folder (packaging Phase 4, plan section 4.5).

    dve_package_game --project <game project> --output <folder>
                     (--runtime-prefix <dve install prefix> | --build-dir <dve build folder>)
                     [--name <Game>] [--tgz] [--verify] [--materials <lib.dvematerials>]
                     [--dve-pack <exe>] [--dve-export-scene <exe>] [--notices <file>]
                     [--strict-export] [--include-sample-maps] [--keep-work]

Steps
  1. Validate <project>/game.dvegame (DVE_GAME 1; name, version, entryScene=*.dvoxscene.json).
  2. Stage the content: every file of the project except editor-only data (.autosave/, .git/,
     editor/ docs/ tests/ artifacts/, project.dveproject, editor revision folders
     *.objects.rN/, audio/sample_maps/ unless --include-sample-maps (decision D8)).
     Every editor scene (*.dvescene) is exported with dve_export_scene to
     <same folder>/<stem>.dvoxscene.json + <stem>.objects/*.dvox, unless the project already has
     that .dvoxscene.json (already cooked: it is used as-is).
  3. dve_pack <stage> <output>/game.dvepak --all (dve_pack strips editor-only paths again).
  4. Copy the installed Runtime component: bin/dve_player becomes <output>/<Game>, and the
     shared libraries it needs from <prefix>/lib/dve go to <output>/lib/dve (the player's RPATH
     includes $ORIGIN/lib/dve, so nothing is needed on LD_LIBRARY_PATH).
  5. Copy THIRD_PARTY_NOTICES (the Runtime component's, share/doc/dve/THIRD_PARTY_NOTICES-
     runtime.txt) and check it lists every shipped shared library; copy the project's own
     LICENSE*/COPYING* files if it has any.
  6. Write build-info.json (engine version, game name/version, pak hash, files, options).
  7. --tgz: also write <Game>-<version>-linux-x86_64.tar.gz next to <output> (reproducible:
     sorted, root-owned, mtime $SOURCE_DATE_EPOCH or 0). The executable is stripped unless
     --no-strip.
  8. --verify: run <output>/<Game> --frames 2 --hash headless from another folder with a clean
     environment (no LD_LIBRARY_PATH) and require "dve_player: PASS".

--build-dir installs the Runtime and RuntimeDeps components of a DVE build folder into a
scratch prefix first (cmake --install), and takes dve_pack/dve_export_scene from that build.
Exit codes: 0 ok, 1 packaging failed, 2 usage error.
"""
from __future__ import annotations

import argparse
import gzip
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

EDITOR_ONLY_DIRS = {".autosave", ".git", ".svn", "__pycache__"}
EDITOR_ONLY_TOP = {"editor", "docs", "tests", "artifacts"}
EDITOR_ONLY_FILES = {"project.dveproject"}
REVISION_DIR = re.compile(r"\.objects\.r[0-9]+$")
NAME_SAFE = re.compile(r"[^A-Za-z0-9._-]+")
BUNDLED_MARK = "Bundled file: "


class PackageError(Exception):
    pass


def log(message: str) -> None:
    print(f"dve_package_game: {message}", flush=True)


def run(command, **kwargs) -> subprocess.CompletedProcess:
    proc = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if proc.returncode != 0:
        raise PackageError(f"{Path(str(command[0])).name} failed ({proc.returncode}):\n{proc.stdout}{proc.stderr}")
    return proc


# ------------------------------------------------------------------------------------------
# 1. manifest
# ------------------------------------------------------------------------------------------
def read_manifest(project: Path) -> dict:
    path = project / "game.dvegame"
    if not path.is_file():
        raise PackageError(f"{path} not found (a game project needs game.dvegame)")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0].strip() != "DVE_GAME 1":
        raise PackageError(f"{path}: first line must be 'DVE_GAME 1'")
    values: dict[str, str] = {}
    for number, raw in enumerate(lines[1:], start=2):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise PackageError(f"{path}:{number}: expected key=value")
        key, value = line.split("=", 1)
        value = re.sub(r"\s+#.*$", "", value).strip()
        if key in values:
            raise PackageError(f"{path}:{number}: duplicate key {key}")
        values[key.strip()] = value
    for key in ("name", "version", "entryScene"):
        if not values.get(key):
            raise PackageError(f"{path}: missing {key}=")
    if not values["entryScene"].endswith(".dvoxscene.json"):
        raise PackageError(f"{path}: entryScene must name a .dvoxscene.json (export editor scenes to it)")
    return values


# ------------------------------------------------------------------------------------------
# 2. staging + scene export
# ------------------------------------------------------------------------------------------
def stage_content(project: Path, stage: Path, export_scene: Path | None, materials: Path | None,
                  strict: bool, include_sample_maps: bool) -> dict:
    copied = 0
    skipped: list[str] = []
    scenes: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(project):
        current = Path(dirpath)
        relative_dir = current.relative_to(project)
        keep = []
        for d in sorted(dirnames):
            rel = (relative_dir / d).as_posix()
            if (d in EDITOR_ONLY_DIRS or REVISION_DIR.search(d)
                    or (relative_dir == Path(".") and d in EDITOR_ONLY_TOP)
                    or (not include_sample_maps and rel.endswith("audio/sample_maps"))):
                skipped.append(rel + "/")
                continue
            keep.append(d)
        dirnames[:] = keep
        for name in sorted(filenames):
            rel = relative_dir / name
            if name in EDITOR_ONLY_FILES or name.endswith(".tmp"):
                skipped.append(rel.as_posix())
                continue
            if name.endswith(".dvescene"):
                scenes.append(rel)
                continue
            target = stage / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(current / name, target)
            copied += 1
    exported: list[str] = []
    precooked: list[str] = []
    for scene in scenes:
        stem = scene.name[: -len(".dvescene")]
        manifest_rel = scene.parent / f"{stem}.dvoxscene.json"
        if (project / manifest_rel).exists():
            precooked.append(manifest_rel.as_posix())
            log(f"{scene.as_posix()}: using the already cooked {manifest_rel.as_posix()}")
            continue
        if export_scene is None:
            raise PackageError(f"{scene.as_posix()} needs dve_export_scene (pass --dve-export-scene)")
        command = [export_scene, project / scene, stage / manifest_rel]
        if materials:
            command += ["--materials", materials]
        if strict:
            command.append("--strict")
        proc = run(command)
        for line in proc.stderr.splitlines():
            log(f"export {scene.as_posix()}: {line.replace('dve_export_scene: ', '')}")
        exported.append(manifest_rel.as_posix())
    return {"copied": copied, "skipped": skipped, "exported": exported, "precooked": precooked}


# ------------------------------------------------------------------------------------------
# 4. runtime
# ------------------------------------------------------------------------------------------
def needed(path: Path) -> list[str]:
    proc = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True)
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", proc.stdout)


def copy_library(source_dir: Path, name: str, target_dir: Path) -> list[str]:
    """Copy lib/dve/<name> (a soname symlink or file) and what it points to."""
    copied = []
    source = source_dir / name
    target_dir.mkdir(parents=True, exist_ok=True)
    while True:
        target = target_dir / source.name
        if source.is_symlink():
            link = os.readlink(source)
            if target.exists() or target.is_symlink():
                target.unlink()
            os.symlink(link, target)
            copied.append(source.name)
            source = (source.parent / link)
            if source.parent != source_dir:
                raise PackageError(f"{name} links outside lib/dve ({link})")
            continue
        shutil.copy2(source, target)
        copied.append(source.name)
        return copied


def stage_runtime(prefix: Path, output: Path, game_name: str, strip: bool) -> dict:
    player = prefix / "bin" / "dve_player"
    if not player.is_file():
        raise PackageError(f"{player} not found; install the Runtime component (cmake --install <build> "
                           f"--component Runtime and RuntimeDeps) or pass --build-dir")
    bundle_dir = prefix / "lib" / "dve"
    executable = output / game_name
    shutil.copy2(player, executable)
    os.chmod(executable, 0o755)
    if strip and shutil.which("strip"):
        # Like CPack's CPACK_STRIP_FILES; the bundled Debian libraries are already stripped.
        run(["strip", "--strip-unneeded", executable])
    libraries: list[str] = []
    pending = [player]
    seen: set[str] = set()
    while pending:
        current = pending.pop(0)
        for name in needed(current):
            if name in seen:
                continue
            seen.add(name)
            if (bundle_dir / name).exists():
                libraries += copy_library(bundle_dir, name, output / "lib" / "dve")
                pending.append((bundle_dir / name).resolve())
    return {"executable": game_name, "libraries": sorted(set(libraries))}


def player_version(executable: Path) -> str:
    proc = subprocess.run([str(executable), "--version"], capture_output=True, text=True, env=clean_env())
    return proc.stdout.strip() or proc.stderr.strip()


def clean_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("LD_")}
    env.setdefault("SDL_VIDEO_DRIVER", "offscreen")
    env.setdefault("SDL_AUDIO_DRIVER", "dummy")
    return env


# ------------------------------------------------------------------------------------------
# 5. notices
# ------------------------------------------------------------------------------------------
def copy_notices(notices: Path, output: Path, libraries: list[str]) -> list[str]:
    if not notices.is_file():
        raise PackageError(f"{notices} not found (build the dve_third_party_notices target and install "
                           f"the Runtime component, or pass --notices)")
    text = notices.read_text(encoding="utf-8", errors="replace")
    shutil.copy2(notices, output / "THIRD_PARTY_NOTICES.txt")
    listed = {line[len(BUNDLED_MARK):].split()[0].rstrip(",")
              for line in text.splitlines() if line.startswith(BUNDLED_MARK)}
    missing = [lib for lib in libraries
               if not any(lib == s or lib.startswith(s + ".") for s in listed)]
    if missing:
        raise PackageError(f"THIRD_PARTY_NOTICES does not cover the shipped libraries: {', '.join(missing)}")
    # Section headers are "=====\n<name>\n=====", and flagged sections carry a "REVIEW:" line.
    flagged = []
    lines = text.splitlines()
    current = ""
    for index, line in enumerate(lines):
        if (0 < index < len(lines) - 1 and lines[index - 1].startswith("=====")
                and lines[index + 1].startswith("=====") and not line.startswith("=====")):
            current = line.strip()
        elif line.startswith("REVIEW:") and current and current not in flagged:
            flagged.append(current)
    return flagged


# ------------------------------------------------------------------------------------------
# 7. archive
# ------------------------------------------------------------------------------------------
def write_tgz(folder: Path, archive: Path) -> None:
    # Reproducible: sorted entries, root-owned, mtime SOURCE_DATE_EPOCH (default 0).
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", "0") or 0)

    def reset(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mtime = mtime
        return info

    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        paths = sorted(folder.rglob("*"), key=lambda p: p.as_posix())
        tar.add(folder, arcname=folder.name, recursive=False, filter=reset)
        for path in paths:
            tar.add(path, arcname=f"{folder.name}/{path.relative_to(folder).as_posix()}", recursive=False, filter=reset)
    with open(archive, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=mtime) as gz:
        gz.write(buffer.getvalue())


def verify(executable: Path) -> str:
    with tempfile.TemporaryDirectory(prefix="dve_package_verify_") as cwd:
        proc = subprocess.run([str(executable), "--frames", "2", "--hash", "--no-audio", "--size", "160x90",
                               "--render-size", "160x90", "--threads", "2"],
                              capture_output=True, text=True, cwd=cwd, env=clean_env(), timeout=600)
    if proc.returncode != 0 or "dve_player: PASS" not in proc.stdout:
        raise PackageError(f"verification run failed ({proc.returncode}):\n{proc.stdout}{proc.stderr}")
    match = re.search(r"framebuffer_fnv=([0-9a-f]+)", proc.stdout)
    return match.group(1) if match else ""


def package(args) -> int:
    project = Path(args.project).resolve()
    output = Path(args.output).resolve()
    manifest = read_manifest(project)
    game_name = args.name or NAME_SAFE.sub("_", manifest["name"]).strip("._-") or "Game"
    if NAME_SAFE.search(game_name):
        raise PackageError(f"--name {game_name!r} must only use [A-Za-z0-9._-]")

    if output.exists():
        if any(output.iterdir()) and not (output / "build-info.json").is_file():
            raise PackageError(f"{output} exists and is not an earlier dve_package_game output; refusing to replace it")
        shutil.rmtree(output)
    output.mkdir(parents=True)
    work = Path(tempfile.mkdtemp(prefix="dve_package_game_"))
    try:
        # Runtime prefix and tools.
        build_dir = Path(args.build_dir).resolve() if args.build_dir else None
        if build_dir:
            prefix = work / "runtime"
            for component in ("Runtime", "RuntimeDeps"):
                run([args.cmake, "--install", build_dir, "--prefix", prefix, "--component", component])
        elif args.runtime_prefix:
            prefix = Path(args.runtime_prefix).resolve()
        else:
            raise PackageError("pass --runtime-prefix <installed dve> or --build-dir <dve build>")

        def tool(explicit, name):
            if explicit:
                return Path(explicit)
            for candidate in ([build_dir / name] if build_dir else []) + [prefix / "bin" / name]:
                if candidate.is_file():
                    return candidate
            return None

        dve_pack = tool(args.dve_pack, "dve_pack")
        export_scene = tool(args.dve_export_scene, "dve_export_scene")
        if dve_pack is None:
            raise PackageError("dve_pack not found (install the Tools component or pass --dve-pack)")

        log(f"packaging '{manifest['name']}' {manifest['version']} from {project}")
        stage = work / "content"
        stage.mkdir()
        staged = stage_content(project, stage, export_scene, Path(args.materials) if args.materials else None,
                               args.strict_export, args.include_sample_maps)
        for key in ("entryScene", "startupScript", "settings"):
            value = manifest.get(key)
            if value and not (stage / value).is_file():
                raise PackageError(f"game.dvegame {key}={value} is not in the staged content")
        log(f"staged {staged['copied']} files, exported {len(staged['exported'])} editor scene(s), "
            f"skipped {len(staged['skipped'])} editor-only path(s)")

        pak = output / "game.dvepak"
        proc = run([dve_pack, stage, pak, "--all"])
        match = re.search(r"with (\d+) files; hash=([0-9a-fx]+)", proc.stdout)
        pak_files, pak_hash = (int(match.group(1)), match.group(2)) if match else (0, "")
        log(proc.stdout.strip())

        runtime = stage_runtime(prefix, output, game_name, not args.no_strip)
        notices = Path(args.notices) if args.notices else prefix / "share" / "doc" / "dve" / "THIRD_PARTY_NOTICES-runtime.txt"
        flagged = copy_notices(notices, output, runtime["libraries"])
        licenses = []
        for pattern in ("LICENSE*", "COPYING*"):
            for path in sorted(project.glob(pattern)):
                if path.is_file():
                    shutil.copy2(path, output / path.name)
                    licenses.append(path.name)
        if not licenses:
            log("warning: the game project has no LICENSE file; the engine has none either (decision D1) - "
                "this folder is for internal use only")

        version = player_version(output / game_name)
        info = {
            "format": "DVE_GAME_PACKAGE", "version": 1,
            "game": {"name": manifest["name"], "version": manifest["version"], "entryScene": manifest["entryScene"]},
            "engine": version,
            "executable": game_name,
            "pak": {"file": "game.dvepak", "files": pak_files, "packageHash": pak_hash},
            "bundledLibraries": runtime["libraries"],
            "exportedScenes": staged["exported"], "precookedScenes": staged["precooked"],
            "excludedEditorOnly": staged["skipped"],
            "licenses": licenses, "noticesReview": flagged,
            "options": {"includeSampleMaps": args.include_sample_maps, "strictExport": args.strict_export},
        }
        (output / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")

        if args.verify:
            fnv = verify(output / game_name)
            log(f"verified: {game_name} --frames 2 ran from a clean environment (framebuffer_fnv={fnv})")
        if args.tgz:
            archive = output.parent / f"{game_name}-{manifest['version']}-linux-x86_64.tar.gz"
            write_tgz(output, archive)
            log(f"wrote {archive} ({archive.stat().st_size // 1024} KiB)")
        size = sum(p.stat().st_size for p in output.rglob("*") if p.is_file() and not p.is_symlink())
        log(f"wrote {output} ({size // 1024} KiB: {game_name}, game.dvepak, "
            f"{len(runtime['libraries'])} files in lib/dve)")
        if flagged:
            log(f"notices flag for review before distribution: {', '.join(flagged)}")
    finally:
        if args.keep_work:
            log(f"work folder kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--name")
    parser.add_argument("--runtime-prefix")
    parser.add_argument("--build-dir")
    parser.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    parser.add_argument("--dve-pack")
    parser.add_argument("--dve-export-scene")
    parser.add_argument("--materials")
    parser.add_argument("--notices")
    parser.add_argument("--strict-export", action="store_true")
    parser.add_argument("--include-sample-maps", action="store_true")
    parser.add_argument("--tgz", action="store_true")
    parser.add_argument("--no-strip", action="store_true", help="keep symbols in the game executable")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--keep-work", action="store_true")
    args = parser.parse_args()
    try:
        return package(args)
    except PackageError as error:
        print(f"dve_package_game: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
