#!/usr/bin/env python3
"""Generate and verify deterministic DVE source-package manifests.

Manifest outputs are intentionally excluded from the payload hash set, avoiding recursive or stale
self-hashes. Generated build trees, VCS metadata, editor audit state, and Python caches are also
excluded. Verification requires an exact payload path set, byte count, and SHA-256 match.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

PACKAGE = "destructible_voxel_engine_v2_30_navigation_heightmap"
VERSION = "2.30"
MANIFEST_FILES = {
    "MANIFEST.json",
    "MANIFEST.sha256",
    "MANIFEST.txt",
    "CHECKSUMS.sha256",
    "SHA256SUMS.txt",
}
EXCLUDED_DIRS = {".git", ".dve", "build", "__pycache__", ".pytest_cache", ".mypy_cache"}


def payload_paths(root: Path) -> list[Path]:
    result: list[Path] = []
    for current, dirs, files in os.walk(root):
        base = Path(current)
        included_dirs: list[str] = []
        for directory in sorted(dirs):
            if (directory in EXCLUDED_DIRS or directory.startswith("cmake-build-") or
                    directory.startswith("build-") or directory.startswith("build_")):
                continue
            path = base / directory
            if path.is_symlink():
                raise RuntimeError(f"release payload may not contain a symbolic-link directory: {path.relative_to(root)}")
            included_dirs.append(directory)
        dirs[:] = included_dirs
        for name in sorted(files):
            path = base / name
            if path.is_symlink():
                raise RuntimeError(f"release payload may not contain a symbolic-link file: {path.relative_to(root)}")
            if not path.is_file():
                raise RuntimeError(f"release payload contains a non-regular file: {path.relative_to(root)}")
            relative = path.relative_to(root).as_posix()
            if relative in MANIFEST_FILES or name.endswith(".pyc") or name.endswith("~"):
                continue
            if ".dve-ai-" in name and name.endswith(".tmp"):
                continue
            result.append(path)
    return sorted(result, key=lambda p: p.relative_to(root).as_posix())


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_manifest(root: Path) -> dict[str, object]:
    records: list[dict[str, object]] = []
    total = 0
    for path in payload_paths(root):
        size = path.stat().st_size
        total += size
        records.append({"path": path.relative_to(root).as_posix(), "bytes": size, "sha256": sha256(path)})
    return {
        "schema": "dve.release-manifest/v2",
        "package": PACKAGE,
        "version": VERSION,
        "file_count": len(records),
        "total_bytes": total,
        "excluded_generated_directories": sorted(EXCLUDED_DIRS),
        "files": records,
    }


def checksum_text(manifest: dict[str, object]) -> str:
    files = manifest["files"]
    assert isinstance(files, list)
    return "".join(f"{record['sha256']}  ./{record['path']}\n" for record in files)


def manifest_text(manifest: dict[str, object]) -> str:
    files = manifest["files"]
    assert isinstance(files, list)
    header = [
        f"Package: {manifest['package']}",
        f"Version: {manifest['version']}",
        f"Payload files: {manifest['file_count']}",
        f"Payload bytes: {manifest['total_bytes']}",
        "",
    ]
    return "\n".join(header) + "\n".join(str(record["path"]) for record in files) + "\n"


def write(root: Path) -> None:
    manifest = build_manifest(root)
    encoded = (json.dumps(manifest, indent=2, sort_keys=False) + "\n").encode("utf-8")
    (root / "MANIFEST.json").write_bytes(encoded)
    digest = hashlib.sha256(encoded).hexdigest()
    (root / "MANIFEST.sha256").write_text(f"{digest}  MANIFEST.json\n", encoding="utf-8", newline="\n")
    checksums = checksum_text(manifest)
    (root / "CHECKSUMS.sha256").write_text(checksums, encoding="utf-8", newline="\n")
    (root / "SHA256SUMS.txt").write_text(checksums, encoding="utf-8", newline="\n")
    (root / "MANIFEST.txt").write_text(manifest_text(manifest), encoding="utf-8", newline="\n")


def verify(root: Path) -> list[str]:
    errors: list[str] = []
    try:
        raw = (root / "MANIFEST.json").read_bytes()
        manifest = json.loads(raw)
    except Exception as exc:
        return [f"MANIFEST.json is missing or invalid: {exc}"]
    if manifest.get("schema") != "dve.release-manifest/v2":
        errors.append("manifest schema is not dve.release-manifest/v2")
    if manifest.get("package") != PACKAGE:
        errors.append(f"package mismatch: {manifest.get('package')!r}")
    if manifest.get("version") != VERSION:
        errors.append(f"version mismatch: {manifest.get('version')!r}")
    expected = build_manifest(root)
    actual_records = manifest.get("files")
    if not isinstance(actual_records, list):
        return errors + ["manifest files field is not an array"]
    actual_by_path = {record.get("path"): record for record in actual_records if isinstance(record, dict)}
    expected_by_path = {record["path"]: record for record in expected["files"]}
    missing = sorted(set(expected_by_path) - set(actual_by_path))
    extra = sorted(set(actual_by_path) - set(expected_by_path))
    if missing:
        errors.append("manifest is missing payload files: " + ", ".join(missing[:20]))
    if extra:
        errors.append("manifest contains absent or excluded files: " + ", ".join(extra[:20]))
    for name in sorted(set(expected_by_path) & set(actual_by_path)):
        expected_record = expected_by_path[name]
        actual_record = actual_by_path[name]
        if actual_record.get("bytes") != expected_record["bytes"]:
            errors.append(f"size mismatch: {name}")
        if actual_record.get("sha256") != expected_record["sha256"]:
            errors.append(f"SHA-256 mismatch: {name}")
    if manifest.get("file_count") != expected["file_count"]:
        errors.append("file_count does not match the payload")
    if manifest.get("total_bytes") != expected["total_bytes"]:
        errors.append("total_bytes does not match the payload")
    expected_manifest_digest = hashlib.sha256(raw).hexdigest() + "  MANIFEST.json\n"
    try:
        if (root / "MANIFEST.sha256").read_text(encoding="utf-8") != expected_manifest_digest:
            errors.append("MANIFEST.sha256 does not match MANIFEST.json")
    except OSError as exc:
        errors.append(f"MANIFEST.sha256 is missing: {exc}")
    expected_checksums = checksum_text(expected)
    for filename in ("CHECKSUMS.sha256", "SHA256SUMS.txt"):
        try:
            if (root / filename).read_text(encoding="utf-8") != expected_checksums:
                errors.append(f"{filename} is stale or malformed")
        except OSError as exc:
            errors.append(f"{filename} is missing: {exc}")
    try:
        if (root / "MANIFEST.txt").read_text(encoding="utf-8") != manifest_text(expected):
            errors.append("MANIFEST.txt is stale")
    except OSError as exc:
        errors.append(f"MANIFEST.txt is missing: {exc}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.write:
        write(root)
    errors = verify(root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    manifest = json.loads((root / "MANIFEST.json").read_text(encoding="utf-8"))
    print(f"verified {manifest['file_count']} payload files ({manifest['total_bytes']} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
