#!/usr/bin/env python3
"""Generate and check THIRD_PARTY_NOTICES for DVE packages (packaging Phase 4).

Inputs
  --manifest third_party/notices/manifest.json   what each third-party library is, its license,
                                                 and where its license text comes from
  --inputs <component>.json                       written by cmake/DveNotices.cmake: the programs
                                                 of one install component, the third-party
                                                 targets/archives in their link closure and the
                                                 DVE sources compiled into them

For each program the shared libraries it will ship with are found the way
install(RUNTIME_DEPENDENCY_SET) finds them: DT_NEEDED is walked recursively (readelf), names
matching the system exclude list are pruned before resolution, the rest resolve through ldd.
Those are the libraries copied into lib/dve for the archive packages.

Modes
  (default)             write --output (a THIRD_PARTY_NOTICES text file)
  --check               exit 1 if a bundled shared library, a linked third-party target or a
                        static archive has no manifest entry, or a used entry's license text
                        or the engine's own LICENSE (manifest "engine") cannot be found (the
                        file is still written when --output is given)
  --verify-dir DIR      exit 1 unless every shared library file in DIR is listed in the
                        notices file given by --notices (used on staged packages)
  --self-test           run the built-in tests (no build needed)

License texts are never invented: they are read from the Debian package that owns the
resolved library (/usr/share/doc/<pkg>/copyright, plus any /usr/share/common-licenses file it
refers to), from the vendored/fetched source tree, or from an SDK folder.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

BUNDLED_MARK = "Bundled file: "


# ------------------------------------------------------------------------------------------
# ELF dependency walk
# ------------------------------------------------------------------------------------------
def readelf_needed(path: str) -> list[str]:
    try:
        out = subprocess.run(["readelf", "-d", path], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)


def ldd_map(path: str) -> dict[str, str]:
    try:
        out = subprocess.run(["ldd", path], check=False, capture_output=True, text=True).stdout
    except OSError:
        return {}
    result = {}
    for line in out.splitlines():
        m = re.match(r"\s*(\S+)\s+=>\s+(/\S+)\s+\(", line)
        if m:
            result[m.group(1)] = m.group(2)
        elif re.match(r"\s*(\S+)\s+=>\s+not found", line):
            result[line.split()[0]] = ""
    return result


def walk_shared_dependencies(program: str, excludes: list[re.Pattern]):
    """Returns (bundled [(soname, path)], system [(soname, path)], unresolved [soname])."""
    resolution = ldd_map(program)
    bundled: dict[str, str] = {}
    system: dict[str, str] = {}
    unresolved: list[str] = []
    pending = [program]
    visited: set[str] = set()
    while pending:
        current = pending.pop(0)
        if current in visited:
            continue
        visited.add(current)
        for name in readelf_needed(current):
            if any(p.search(name) for p in excludes):
                system.setdefault(name, resolution.get(name, ""))
                continue
            path = resolution.get(name, "")
            if not path:
                if name not in unresolved:
                    unresolved.append(name)
                continue
            if name not in bundled:
                bundled[name] = path
                pending.append(path)
    return sorted(bundled.items()), sorted(system.items()), unresolved


# ------------------------------------------------------------------------------------------
# Debian metadata
# ------------------------------------------------------------------------------------------
_dpkg_cache: dict = {}


def debian_package(path: str):
    """(binary package, source package, source version) owning `path`, or None."""
    if not path:
        return None
    if path in _dpkg_cache:
        return _dpkg_cache[path]
    result = None
    if shutil.which("dpkg"):
        candidates = [path, os.path.realpath(path)]
        # Merged /usr: dpkg may record /lib/x86_64-linux-gnu or /usr/lib/x86_64-linux-gnu.
        for c in list(candidates):
            if c.startswith("/usr/lib/"):
                candidates.append(c[len("/usr"):])
            elif c.startswith("/lib/"):
                candidates.append("/usr" + c)
        for candidate in candidates:
            proc = subprocess.run(["dpkg", "-S", candidate], capture_output=True, text=True)
            if proc.returncode == 0:
                m = re.match(r"^([a-z0-9][a-z0-9+.-]+)(:[a-z0-9-]+)?: ", proc.stdout)
                if m:
                    pkg = m.group(1)
                    q = subprocess.run(["dpkg-query", "-W", "-f=${source:Package}\t${source:Version}", pkg],
                                       capture_output=True, text=True)
                    src, ver = (q.stdout.split("\t") + ["", ""])[:2] if q.returncode == 0 else ("", "")
                    result = (pkg, src or pkg, ver)
                    break
    _dpkg_cache[path] = result
    return result


# ------------------------------------------------------------------------------------------
# Manifest matching
# ------------------------------------------------------------------------------------------
@dataclass
class Use:
    how: str            # bundled | static | target | adaptation | system
    what: str           # soname, target name, archive name or source file
    path: str = ""      # resolved file (shared libraries/archives)
    program: str = ""


@dataclass
class EntryUse:
    entry: dict
    uses: list = field(default_factory=list)


def compile_entry(entry: dict) -> dict:
    match = entry.get("match", {})
    return {
        "sonames": [re.compile(p) for p in match.get("sonames", [])],
        "targets": [re.compile(p) for p in match.get("targets", [])],
        "archives": [re.compile(p) for p in match.get("archives", [])],
        "sources": list(match.get("sources", [])),
    }


def find_entry(entries, compiled, kind: str, value: str):
    for entry, c in zip(entries, compiled):
        if kind == "sources":
            if any(fnmatch.fnmatch(value, pattern) for pattern in c["sources"]):
                return entry
        elif any(p.search(value) for p in c[kind]):
            return entry
    return None


# ------------------------------------------------------------------------------------------
# License texts
# ------------------------------------------------------------------------------------------
def read_text(path: Path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


def search_up(start: str, names, limit: int = 6):
    if not start:
        return None
    current = Path(start)
    for _ in range(limit):
        for name in names:
            candidate = current / name
            if candidate.is_file():
                return candidate
        if current.name.endswith("-src") or current.parent == current:
            break
        current = current.parent
    return None


def resolve_texts(entry: dict, uses, inputs: dict, source_dir: Path):
    """[(title, text)], problems."""
    texts = []
    problems = []
    seen = set()

    def add(title: str, path: Path) -> bool:
        key = str(path.resolve()) if path.exists() else str(path)
        if key in seen:
            return True
        text = read_text(path)
        if text is None:
            return False
        seen.add(key)
        texts.append((title, text))
        return True

    target_dirs = {t["name"]: t.get("source_dir", "") for t in inputs.get("targets", [])}
    for spec in entry.get("texts", []):
        if spec.get("debian"):
            for use in uses:
                if use.how not in ("bundled", "static", "system") or not use.path:
                    continue
                pkg = debian_package(use.path)
                if pkg:
                    add(f"Debian copyright file of {pkg[0]}", Path(f"/usr/share/doc/{pkg[0]}/copyright"))
        elif "file" in spec:
            path = Path(spec["file"])
            if path.is_file():
                add(str(path), path)
        elif "repo" in spec:
            path = source_dir / spec["repo"]
            if not add(spec["repo"], path):
                problems.append(f"{entry['id']}: license file {spec['repo']} is missing")
        elif "target_file" in spec:
            for use in uses:
                if use.how != "target":
                    continue
                found = search_up(target_dirs.get(use.what, ""), spec["target_file"])
                if found:
                    add(f"{found.name} from the {use.what} source tree", found)
        elif "var" in spec:
            root = inputs.get("vars", {}).get(spec["var"], "")
            if root:
                for name in spec.get("files", []):
                    if add(f"{name} from {spec['var']}", Path(root) / name):
                        break
    if not texts:
        problems.append(f"{entry['id']} ({entry['name']}): no license text found "
                        f"(tried {', '.join(sorted({k for s in entry.get('texts', []) for k in s}))})")
    return texts, problems


def common_licenses(texts):
    names = []
    for _, text in texts:
        for name in re.findall(r"/usr/share/common-licenses/([A-Za-z0-9.+_-]*[A-Za-z0-9+])", text):
            if name not in names:
                names.append(name)
    return names


# ------------------------------------------------------------------------------------------
# Analysis and rendering
# ------------------------------------------------------------------------------------------
def analyse(manifest: dict, inputs: dict, source_dir: Path):
    entries = manifest["entries"]
    compiled = [compile_entry(e) for e in entries]
    excludes = [re.compile(p) for p in inputs.get("system_excludes", [])]
    used: dict = {}
    unknown = []
    system_libraries: dict = {}
    unresolved = []

    def record(entry, use: Use):
        slot = used.setdefault(entry["id"], EntryUse(entry))
        if not any(u.how == use.how and u.what == use.what for u in slot.uses):
            slot.uses.append(use)

    for program in inputs.get("programs", []):
        file = program["file"]
        if not os.path.isfile(file):
            unknown.append(f"program {program['name']} not built ({file})")
            continue
        bundled, system, missing = walk_shared_dependencies(file, excludes)
        unresolved += [f"{program['name']}: {m}" for m in missing]
        for soname, path in bundled:
            entry = find_entry(entries, compiled, "sonames", soname)
            if entry:
                record(entry, Use("bundled", soname, path, program["name"]))
            else:
                unknown.append(f"bundled shared library {soname} ({path}) used by {program['name']}")
        for soname, path in system:
            system_libraries.setdefault(soname, path)
            entry = find_entry(entries, compiled, "sonames", soname)
            if entry:
                record(entry, Use("system", soname, path, program["name"]))

    for target in inputs.get("targets", []):
        name = target["name"]
        if target.get("imported"):
            # Imported targets are what the build links against; what ships is decided by
            # their files: shared libraries through the ELF walk, archives below.
            continue
        entry = find_entry(entries, compiled, "targets", name)
        if entry:
            record(entry, Use("target", name))
        elif not name.startswith("dve_") and target.get("type") != "INTERFACE_LIBRARY":
            unknown.append(f"third-party target {name} ({target.get('type', '?')}) built in this tree")

    for path in inputs.get("files", []):
        base = os.path.basename(path)
        if base.endswith(".a") or base.endswith(".lib"):
            entry = find_entry(entries, compiled, "archives", path)
            if entry:
                record(entry, Use("static", base, path))
            else:
                unknown.append(f"static archive {path}")

    for source in inputs.get("sources", []):
        entry = find_entry(entries, compiled, "sources", source)
        if entry:
            record(entry, Use("adaptation", source))

    return used, unknown, system_libraries, unresolved


HOW_TEXT = {
    "bundled": "bundled shared library (lib/dve in the archive packages)",
    "static": "statically linked archive",
    "target": "built from source and linked statically",
    "adaptation": "concepts/algorithms adapted in DVE source compiled into this component",
    "system": "system library, linked but NOT redistributed",
}


def render(manifest: dict, inputs: dict, used: dict, system_libraries: dict, unknown, source_dir: Path):
    component = inputs.get("component", "?")
    version = inputs.get("project_version", "?")
    problems = []
    order = [e["id"] for e in manifest["entries"]]
    shipped = [used[i] for i in order if i in used and any(u.how != "system" for u in used[i].uses)]
    system_only = [used[i] for i in order if i in used and all(u.how == "system" for u in used[i].uses)]
    resolved = {}
    for slot in shipped:
        texts, text_problems = resolve_texts(slot.entry, slot.uses, inputs, source_dir)
        resolved[slot.entry["id"]] = texts
        problems += text_problems

    lines = []
    lines.append(f"THIRD-PARTY SOFTWARE NOTICES: DVE {version}, component {component}")
    lines.append("=" * 78)
    lines.append("Generated by tools/generate_third_party_notices.py from third_party/notices/manifest.json.")
    lines.append("Do not edit; it is regenerated by the dve_third_party_notices build target.")
    lines.append("")
    engine = manifest.get("engine")
    if not engine:
        problems.append("the manifest has no 'engine' entry (the engine's own license)")
    else:
        engine_text = read_text(source_dir / engine.get("text", "LICENSE"))
        if engine_text is None:
            problems.append(f"engine license text not found: {source_dir / engine.get('text', 'LICENSE')}")
        lines.append(f"{engine['name']} is licensed under the {engine['license']} License:")
        if engine.get("copyright"):
            lines.append(engine["copyright"])
        if engine.get("upstream"):
            lines.append(f"Source: {engine['upstream']}")
        lines.append("The notices after the engine's license cover the third-party software in this")
        lines.append("component; their licenses apply to those parts, not the engine's MIT license.")
        lines.append("")
        lines.append(f"--- {engine['name']}: {engine.get('text', 'LICENSE')} ---")
        lines.append((engine_text or "(missing)").rstrip())
        lines.append("")
    programs = ", ".join(p["name"] for p in inputs.get("programs", [])) or "(libraries only)"
    lines.append(f"Programs: {programs}")
    lines.append("")
    lines.append("Summary")
    lines.append("-------")
    for slot in shipped:
        e = slot.entry
        hows = sorted({u.how for u in slot.uses if u.how != "system"})
        lines.append(f"  {e['name']}")
        lines.append(f"      license: {e['license']}")
        lines.append(f"      how:     {'; '.join(HOW_TEXT[h] for h in hows)}")
        if e.get("copyleft", "none") != "none":
            lines.append(f"      COPYLEFT ({e['copyleft']}): see the notes in its section")
        if e.get("review"):
            lines.append("      REVIEW REQUIRED before external distribution")
    lines.append("")

    offers = [s for s in shipped if s.entry.get("copyleft", "none") != "none"
              and any(u.how == "bundled" for u in s.uses)]
    if offers:
        lines.append("Corresponding source for copyleft components")
        lines.append("--------------------------------------------")
        lines.append("The libraries below are shipped as unmodified shared libraries from the Debian")
        lines.append("packages named here. Their complete corresponding source is the Debian source package")
        lines.append("of that version (apt-get source <package>=<version>, or")
        lines.append("https://snapshot.debian.org/package/<package>/<version>/). Whoever distributes this")
        lines.append("package must make that source available (or include a written offer) as the license")
        lines.append("requires; a pointer to snapshot.debian.org alone may not be enough.")
        for slot in offers:
            for use in slot.uses:
                if use.how == "bundled":
                    pkg = debian_package(use.path)
                    src = f"Debian source package {pkg[1]} {pkg[2]} (binary {pkg[0]})" if pkg else "source package unknown"
                    lines.append(f"  {use.what}: {src}")
        lines.append("")

    for slot in shipped:
        e = slot.entry
        lines.append("=" * 78)
        lines.append(e["name"])
        lines.append("=" * 78)
        lines.append(f"License: {e['license']}")
        if e.get("upstream"):
            lines.append(f"Upstream: {e['upstream']}")
        for use in slot.uses:
            if use.how == "system":
                continue
            if use.how == "bundled":
                pkg = debian_package(use.path)
                origin = f" (from Debian package {pkg[0]}, source {pkg[1]} {pkg[2]})" if pkg else f" ({use.path})"
                lines.append(f"{BUNDLED_MARK}{use.what}{origin}, needed by {use.program}")
            elif use.how == "adaptation":
                lines.append(f"Adapted in: {use.what}")
            else:
                lines.append(f"Linked: {use.what} ({HOW_TEXT[use.how]})")
        if e.get("notes"):
            lines.append(f"Note: {e['notes']}")
        if e.get("review"):
            lines.append(f"REVIEW: {e['review']}")
        for title, text in resolved[e["id"]]:
            lines.append("")
            lines.append(f"--- {title} ---")
            lines.append(text.rstrip())
        lines.append("")

    # Full texts of the common licenses the Debian copyright files refer to.
    referenced = common_licenses([t for texts in resolved.values() for t in texts])
    if referenced:
        lines.append("=" * 78)
        lines.append("Appendix: license texts referenced above (/usr/share/common-licenses)")
        lines.append("=" * 78)
        for name in referenced:
            text = read_text(Path("/usr/share/common-licenses") / name)
            if text is None:
                continue
            lines.append(f"--- {name} ---")
            lines.append(text.rstrip())
            lines.append("")

    lines.append("=" * 78)
    lines.append("System libraries (linked, not redistributed; provided by the operating system)")
    lines.append("=" * 78)
    for soname in sorted(system_libraries):
        pkg = debian_package(system_libraries[soname])
        entry = next((s.entry for s in system_only + shipped if any(u.what == soname for u in s.uses)), None)
        license_note = f", {entry['license']}" if entry else ""
        lines.append(f"  {soname}" + (f" (Debian package {pkg[0]}{license_note})" if pkg else license_note))
    if unknown:
        lines.append("")
        lines.append("UNRESOLVED (no manifest entry; the dve_third_party_notices_check test fails):")
        for item in unknown:
            lines.append(f"  {item}")
    lines.append("")
    return "\n".join(lines), problems


def listed_bundled(notices_text: str):
    names = set()
    for line in notices_text.splitlines():
        if line.startswith(BUNDLED_MARK):
            names.add(line[len(BUNDLED_MARK):].split()[0].rstrip(","))
    return names


def verify_dir(directory: Path, notices: Path):
    listed = listed_bundled(notices.read_text(encoding="utf-8", errors="replace"))
    problems = []
    for path in sorted(directory.iterdir()) if directory.is_dir() else []:
        if not path.is_file() and not path.is_symlink():
            continue
        name = path.name
        if ".so" not in name and not name.endswith(".dll") and not name.endswith(".dylib"):
            continue
        if not any(name == soname or name.startswith(soname + ".") for soname in listed):
            problems.append(f"{path} is shipped but not listed in {notices.name}")
    return problems


def run(args) -> int:
    manifest = json.loads(Path(args.manifest).read_text())
    inputs = json.loads(Path(args.inputs).read_text())
    source_dir = Path(args.source_dir or inputs.get("source_dir", "."))
    used, unknown, system_libraries, unresolved = analyse(manifest, inputs, source_dir)
    text, problems = render(manifest, inputs, used, system_libraries, unknown, source_dir)
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        if not out.exists() or out.read_text(errors="replace") != text:
            out.write_text(text)
    failures = [f"no manifest entry: {u}" for u in unknown] + problems
    failures += [f"unresolved shared library: {u}" for u in unresolved]
    component = inputs.get("component", "?")
    shipped = [i for i, s in used.items() if any(u.how != "system" for u in s.uses)]
    flagged = [used[i].entry["id"] for i in shipped
               if used[i].entry.get("copyleft", "none") != "none" or used[i].entry.get("review")]
    print(f"notices[{component}]: {len(shipped)} third-party entries "
          f"({', '.join(shipped)}); flagged: {', '.join(flagged) or 'none'}")
    if args.check and failures:
        for f in failures:
            print(f"notices[{component}]: FAIL: {f}", file=sys.stderr)
        return 1
    for f in failures:
        print(f"notices[{component}]: warning: {f}", file=sys.stderr)
    return 0


# ------------------------------------------------------------------------------------------
# Self-test
# ------------------------------------------------------------------------------------------
def self_test() -> int:
    failures = 0

    def check(condition: bool, what: str):
        nonlocal failures
        if not condition:
            failures += 1
            print(f"FAIL: {what}", file=sys.stderr)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "third_party").mkdir()
        (root / "third_party" / "FOO-LICENSE.txt").write_text("Foo license text\n")
        (root / "deps" / "bar-src" / "sub").mkdir(parents=True)
        (root / "deps" / "bar-src" / "LICENSE").write_text("Bar license text\n")
        (root / "LICENSE").write_text("MIT License\n\nEngine license text\n")
        manifest = {"engine": {"name": "Test Engine", "license": "MIT", "copyright": "Copyright (c) 2026 Test",
                               "text": "LICENSE"}, "entries": [
            {"id": "foo", "name": "Foo", "license": "MIT", "copyleft": "none",
             "match": {"sources": ["src/foo_*.cpp"]}, "texts": [{"repo": "third_party/FOO-LICENSE.txt"}]},
            {"id": "bar", "name": "Bar", "license": "Zlib", "copyleft": "weak", "review": "check me",
             "match": {"targets": ["^bar$"], "archives": ["libbar\\.a$"]}, "texts": [{"target_file": ["LICENSE"]}]},
            {"id": "gone", "name": "Gone", "license": "MIT", "match": {"targets": ["^gone$"]},
             "texts": [{"repo": "third_party/MISSING.txt"}]},
        ]}
        base = {"component": "Test", "project_version": "0.0.0", "source_dir": str(root),
                "system_excludes": [], "programs": []}

        def analyse_render(inputs):
            used, unknown, system, _ = analyse(manifest, inputs, root)
            text, problems = render(manifest, inputs, used, system, unknown, root)
            return used, unknown, text, problems

        ok = dict(base, targets=[{"name": "bar", "type": "STATIC_LIBRARY", "source_dir": str(root / "deps/bar-src/sub")}],
                  files=["/x/libbar.a"], sources=["src/foo_solver.cpp", "src/unrelated.cpp"])
        used, unknown, text, problems = analyse_render(ok)
        check(set(used) == {"foo", "bar"}, "known target, archive and source are matched")
        check(not unknown and not problems, f"no problems for a covered closure ({unknown}, {problems})")
        check("Foo license text" in text and "Bar license text" in text, "license texts are included")
        check("licensed under the MIT License" in text and "Engine license text" in text,
              "the engine's MIT license and its text are stated")
        check("NO LICENSE" not in text, "the old no-license statement is gone")
        check("REVIEW REQUIRED" in text and "COPYLEFT (weak)" in text, "copyleft/review flags are shown")

        bad = dict(base, targets=[{"name": "mystery", "type": "STATIC_LIBRARY"}], files=["/x/libmystery.a"], sources=[])
        _, unknown, text, _ = analyse_render(bad)
        check(any("mystery" in u and "target" in u for u in unknown), "an unknown in-tree target is reported")
        check(any("libmystery.a" in u for u in unknown), "an unknown static archive is reported")
        check("UNRESOLVED" in text, "unresolved items are listed in the notices")

        dve_only = dict(base, targets=[{"name": "dve_core", "type": "STATIC_LIBRARY"},
                                        {"name": "Threads::Threads", "type": "INTERFACE_LIBRARY", "imported": True}],
                        files=[], sources=[])
        _, unknown, _, _ = analyse_render(dve_only)
        check(not unknown, "DVE's own and imported interface targets need no entry")

        missing = dict(base, targets=[{"name": "gone", "type": "STATIC_LIBRARY"}], files=[], sources=[])
        _, _, _, problems = analyse_render(missing)
        check(any("MISSING.txt" in p for p in problems), "a missing license file is a problem")

        (root / "LICENSE").unlink()
        _, _, text, problems = analyse_render(ok)
        check(any("engine license text not found" in p for p in problems), "a missing engine LICENSE is a problem")
        (root / "LICENSE").write_text("MIT License\n\nEngine license text\n")

        # --verify-dir: every shipped .so must be listed.
        notices = root / "NOTICES.txt"
        notices.write_text(f"{BUNDLED_MARK}libfoo.so.1 (x), needed by p\n")
        lib = root / "lib"
        lib.mkdir()
        (lib / "libfoo.so.1.2.3").write_text("")
        check(not verify_dir(lib, notices), "a listed soname covers its versioned file")
        (lib / "libsecret.so.2").write_text("")
        check(any("libsecret" in p for p in verify_dir(lib, notices)), "an unlisted shipped library is reported")

        # ELF walk on a real binary when the tools exist.
        if shutil.which("readelf") and shutil.which("ldd") and os.path.exists("/bin/ls"):
            bundled, system, _ = walk_shared_dependencies("/bin/ls", [re.compile(r"^libc\.so")])
            check(any(n.startswith("libc.so") for n, _ in system), "excluded libraries are reported as system")
            check(all(not n.startswith("libc.so") for n, _ in bundled), "excluded libraries are not bundled")
    if failures:
        print(f"generate_third_party_notices self-test: {failures} failure(s)", file=sys.stderr)
        return 1
    print("generate_third_party_notices self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest")
    parser.add_argument("--inputs")
    parser.add_argument("--output")
    parser.add_argument("--source-dir")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--verify-dir")
    parser.add_argument("--notices")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.verify_dir:
        if not args.notices:
            parser.error("--verify-dir needs --notices")
        problems = verify_dir(Path(args.verify_dir), Path(args.notices))
        for p in problems:
            print(f"FAIL: {p}", file=sys.stderr)
        if not problems:
            print(f"every shared library in {args.verify_dir} is listed in {args.notices}")
        return 1 if problems else 0
    if not args.manifest or not args.inputs:
        parser.error("--manifest and --inputs are required")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
