#!/usr/bin/env python3
"""dve_package_game: build a shippable game folder (packaging Phase 4, plan section 4.5).

    dve_package_game --project <game project> --output <folder>
                     (--runtime-prefix <dve install prefix> | --build-dir <dve build folder>)
                     [--name <Game>] [--tgz] [--verify] [--materials <lib.dvematerials>]
                     [--dve-pack <exe>] [--dve-export-scene <exe>] [--notices <file>]
                     [--engine-license <file>]
                     [--strict-export] [--include-sample-maps] [--allow-mp3-libraries] [--keep-work]
                     [--zip] [--config <cfg>]

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
     A runtime that bundles libmpg123/libmp3lame (a libsndfile with MPEG support) is refused
     unless --allow-mp3-libraries; DVE's default libsndfile build has no MPEG support.
  5. Copy THIRD_PARTY_NOTICES (the Runtime component's, share/doc/dve/THIRD_PARTY_NOTICES-
     runtime.txt) and check it lists every shipped shared library; copy the engine's MIT license
     (share/doc/dve-runtime/LICENSE) to <output>/DVE-LICENSE.txt next to it; copy the project's
     own LICENSE*/COPYING* files if it has any.
  6. Write build-info.json (engine version, game name/version, pak hash, files, options).
  7. --tgz: also write <Game>-<version>-linux-x86_64.tar.gz next to <output> (reproducible:
     sorted, root-owned, mtime $SOURCE_DATE_EPOCH or 0). The executable is stripped unless
     --no-strip.
  8. --verify: run <output>/<Game> --frames 2 --hash headless from another folder with a clean
     environment (no LD_LIBRARY_PATH) and require "dve_player: PASS".

Windows (the runtime prefix has bin/dve_player.exe): the player becomes <output>/<Game>.exe and
the DLLs it imports (read from its PE import table, recursively) are copied from <prefix>/bin
next to it, including the Visual C++ runtime the Runtime component installs; Windows system
DLLs are not copied. Nothing is stripped. --zip writes <Game>-<version>-windows-x86_64.zip
next to <output> (sorted entries, fixed timestamps). --verify runs <Game>.exe with PATH reduced
to the Windows folders, so a DLL that is not in the game folder makes it fail. --config picks
the configuration --build-dir installs (multi-config generators such as Visual Studio).

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
import struct
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
ENGINE_LICENSE_NAME = "DVE-LICENSE.txt"


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


def pe_imports(path: Path) -> list[str]:
    """DLL names a Windows .exe/.dll imports (import and delay-import tables). The same reader
    as tools/generate_third_party_notices.py, kept here so the installed script stands alone."""
    data = Path(path).read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        return []
    try:
        header = struct.unpack_from("<I", data, 0x3C)[0]
        if data[header:header + 4] != b"PE\0\0":
            return []
        coff = header + 4
        section_count = struct.unpack_from("<H", data, coff + 2)[0]
        optional_size = struct.unpack_from("<H", data, coff + 16)[0]
        optional = coff + 20
        magic = struct.unpack_from("<H", data, optional)[0]
        count_offset, directories = (optional + 92, optional + 96) if magic == 0x10B else (optional + 108, optional + 112)
        directory_count = struct.unpack_from("<I", data, count_offset)[0]
        sections = []
        for index in range(section_count):
            entry = optional + optional_size + 40 * index
            virtual_size, address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, entry + 8)
            sections.append((address, max(virtual_size, raw_size), raw_pointer, raw_size))

        def offset(rva: int):
            for address, size, raw_pointer, raw_size in sections:
                if address <= rva < address + size and rva - address < raw_size:
                    return raw_pointer + rva - address
            return None

        names: list[str] = []
        for index, size, name_field in ((1, 20, 3), (13, 32, 1)):
            rva = struct.unpack_from("<I", data, directories + 8 * index)[0] if index < directory_count else 0
            position = offset(rva) if rva else None
            while position is not None and position + size <= len(data):
                fields = struct.unpack_from("<" + "I" * (size // 4), data, position)
                if not any(fields):
                    break
                start = offset(fields[name_field])
                if start is not None:
                    name = data[start:data.find(b"\0", start)].decode("ascii", "replace")
                    if name and name not in names:
                        names.append(name)
                position += size
        return names
    except (struct.error, ValueError):
        return []


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


def is_windows_runtime(prefix: Path) -> bool:
    return (prefix / "bin" / "dve_player.exe").is_file()


def executable_name(game_name: str, windows: bool) -> str:
    return f"{game_name}.exe" if windows else game_name


def stage_windows_runtime(prefix: Path, output: Path, game_name: str) -> dict:
    """bin/dve_player.exe -> <output>/<Game>.exe, plus every DLL it imports (recursively) that
    the Runtime/RuntimeDeps components installed in bin/. Anything else is a Windows DLL."""
    bin_dir = prefix / "bin"
    available = {p.name.lower(): p for p in bin_dir.iterdir() if p.is_file() and p.suffix.lower() == ".dll"}
    executable = output / executable_name(game_name, True)
    shutil.copy2(bin_dir / "dve_player.exe", executable)
    libraries: list[str] = []
    pending = [bin_dir / "dve_player.exe"]
    seen: set[str] = set()
    while pending:
        current = pending.pop(0)
        for name in pe_imports(current):
            key = name.lower()
            if key in seen:
                continue
            seen.add(key)
            if key in available:
                shutil.copy2(available[key], output / available[key].name)
                libraries.append(available[key].name)
                pending.append(available[key])
    return {"executable": executable.name, "libraries": sorted(libraries, key=str.lower)}


def stage_runtime(prefix: Path, output: Path, game_name: str, strip: bool) -> dict:
    if is_windows_runtime(prefix):
        return stage_windows_runtime(prefix, output, game_name)
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


# libmpg123 / libmp3lame only come with a libsndfile built with MPEG support (Debian's). DVE's
# own build (DVE_FETCH_SNDFILE, the default) has none, and DVE needs no MP3 at run time, so a
# runtime prefix that bundles them is refused (third_party/notices/manifest.json "forbidden").
FORBIDDEN_LIBRARIES = re.compile(r"^(libmpg123|libmp3lame)\.so|^(lib)?(mpg123|mp3lame)([-_.][0-9]+)?\.dll$", re.IGNORECASE)


def check_forbidden(libraries: list[str], allow: bool) -> list[str]:
    forbidden = sorted(lib for lib in libraries if FORBIDDEN_LIBRARIES.match(lib))
    if forbidden and not allow:
        raise PackageError(
            f"the runtime bundles MP3 libraries ({', '.join(forbidden)}): it was built against a libsndfile "
            f"with MPEG support. Rebuild DVE with -DDVE_FETCH_SNDFILE=ON (the default) so libsndfile is built "
            f"without MPEG, or pass --allow-mp3-libraries after reviewing their licenses (LGPL; lame's fft.c "
            f"is marked GPL-1+ by Debian)")
    return forbidden


def player_version(executable: Path) -> str:
    proc = subprocess.run([str(executable), "--version"], capture_output=True, text=True, env=clean_env())
    return proc.stdout.strip() or proc.stderr.strip()


def clean_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("LD_")}
    if os.name == "nt":
        # Only the Windows folders: a DLL the game folder lacks must make the run fail.
        root = os.environ.get("SystemRoot", r"C:\Windows")
        for key in [k for k in env if k.upper() == "PATH"]:
            del env[key]
        env["PATH"] = os.pathsep.join([os.path.join(root, "System32"), root, os.path.join(root, "System32", "Wbem")])
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
               if not any(lib == s or lib.startswith(s + ".") or (lib.lower().endswith(".dll") and lib.lower() == s.lower())
                          for s in listed)]
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
def write_zip(folder: Path, archive: Path) -> None:
    # Reproducible: sorted entries, fixed timestamps (SOURCE_DATE_EPOCH, at least 1980-01-01).
    import time
    import zipfile
    epoch = max(int(os.environ.get("SOURCE_DATE_EPOCH", "0") or 0), 315532800)
    stamp = time.gmtime(epoch)[:6]

    def entry(name: str, directory: bool) -> zipfile.ZipInfo:
        info = zipfile.ZipInfo(name + ("/" if directory else ""), date_time=stamp)
        info.create_system = 0
        info.external_attr = 0x10 if directory else 0
        info.compress_type = zipfile.ZIP_STORED if directory else zipfile.ZIP_DEFLATED
        return info

    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        zf.writestr(entry(folder.name, True), b"")
        for path in sorted(folder.rglob("*"), key=lambda p: p.as_posix()):
            name = f"{folder.name}/{path.relative_to(folder).as_posix()}"
            if path.is_dir():
                zf.writestr(entry(name, True), b"")
            else:
                zf.writestr(entry(name, False), path.read_bytes())


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
    if args.tool_path:
        os.environ["PATH"] = os.pathsep.join([*args.tool_path, os.environ.get("PATH", "")])
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
            config = ["--config", args.config] if args.config else []
            for component in ("Runtime", "RuntimeDeps"):
                run([args.cmake, "--install", build_dir, "--prefix", prefix, "--component", component, *config])
        elif args.runtime_prefix:
            prefix = Path(args.runtime_prefix).resolve()
        else:
            raise PackageError("pass --runtime-prefix <installed dve> or --build-dir <dve build>")

        def tool(explicit, name):
            if explicit:
                return Path(explicit)
            names = [name, f"{name}.exe"] if os.name == "nt" else [name]
            build_dirs = [build_dir] + ([build_dir / args.config] if args.config else []) if build_dir else []
            for candidate in [d / n for d in build_dirs + [prefix / "bin"] for n in names]:
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

        windows = is_windows_runtime(prefix)
        runtime = stage_runtime(prefix, output, game_name, not args.no_strip)
        mp3_libraries = check_forbidden(runtime["libraries"], args.allow_mp3_libraries)
        if mp3_libraries:
            log(f"warning: shipping MP3 libraries (--allow-mp3-libraries): {', '.join(mp3_libraries)}")
        notices = Path(args.notices) if args.notices else prefix / "share" / "doc" / "dve" / "THIRD_PARTY_NOTICES-runtime.txt"
        flagged = copy_notices(notices, output, runtime["libraries"])
        engine_license = (Path(args.engine_license) if args.engine_license
                          else prefix / "share" / "doc" / "dve-runtime" / "LICENSE")
        if not engine_license.is_file():
            raise PackageError(f"{engine_license} not found (the engine's MIT license; install the Runtime "
                               f"component or pass --engine-license)")
        shutil.copy2(engine_license, output / ENGINE_LICENSE_NAME)
        licenses = []
        for pattern in ("LICENSE*", "COPYING*"):
            for path in sorted(project.glob(pattern)):
                if path.is_file():
                    shutil.copy2(path, output / path.name)
                    licenses.append(path.name)
        if not licenses:
            log(f"note: the game project has no LICENSE file; only the engine's MIT license "
                f"({ENGINE_LICENSE_NAME}) and THIRD_PARTY_NOTICES.txt are included")

        version = player_version(output / runtime["executable"])
        info = {
            "format": "DVE_GAME_PACKAGE", "version": 1,
            "game": {"name": manifest["name"], "version": manifest["version"], "entryScene": manifest["entryScene"]},
            "engine": version,
            "executable": runtime["executable"],
            "pak": {"file": "game.dvepak", "files": pak_files, "packageHash": pak_hash},
            "bundledLibraries": runtime["libraries"],
            "exportedScenes": staged["exported"], "precookedScenes": staged["precooked"],
            "excludedEditorOnly": staged["skipped"],
            "engineLicense": {"file": ENGINE_LICENSE_NAME, "license": "MIT"},
            "licenses": licenses, "noticesReview": flagged,
            "options": {"includeSampleMaps": args.include_sample_maps, "strictExport": args.strict_export,
                        "allowMp3Libraries": args.allow_mp3_libraries},
        }
        (output / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")

        if args.verify:
            fnv = verify(output / runtime["executable"])
            log(f"verified: {game_name} --frames 2 ran from a clean environment (framebuffer_fnv={fnv})")
        system = "windows" if windows else "linux"
        if args.tgz:
            archive = output.parent / f"{game_name}-{manifest['version']}-{system}-x86_64.tar.gz"
            write_tgz(output, archive)
            log(f"wrote {archive} ({archive.stat().st_size // 1024} KiB)")
        if args.zip:
            archive = output.parent / f"{game_name}-{manifest['version']}-{system}-x86_64.zip"
            write_zip(output, archive)
            log(f"wrote {archive} ({archive.stat().st_size // 1024} KiB)")
        size = sum(p.stat().st_size for p in output.rglob("*") if p.is_file() and not p.is_symlink())
        log(f"wrote {output} ({size // 1024} KiB: {runtime['executable']}, game.dvepak, "
            f"{len(runtime['libraries'])} {'DLLs' if windows else 'files in lib/dve'})")
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
    parser.add_argument("--engine-license", help="the engine's LICENSE (default: <prefix>/share/doc/dve-runtime/LICENSE)")
    parser.add_argument("--strict-export", action="store_true")
    parser.add_argument("--include-sample-maps", action="store_true")
    parser.add_argument("--allow-mp3-libraries", action="store_true",
                        help="package even if the runtime bundles libmpg123/libmp3lame (refused by default)")
    parser.add_argument("--tgz", action="store_true")
    parser.add_argument("--zip", action="store_true", help="also write <Game>-<version>-<system>-x86_64.zip")
    parser.add_argument("--tool-path", action="append", default=[],
                        help="folder prepended to PATH for dve_pack/dve_export_scene (e.g. vcpkg's bin on Windows); "
                             "not used for --verify")
    parser.add_argument("--config", help="configuration for cmake --install with --build-dir (multi-config generators)")
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
