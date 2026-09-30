#!/usr/bin/env sh
# Regenerates SOURCE_MANIFEST.sha256: the sha256 of every file git knows about (tracked, plus
# untracked files that are not ignored), sorted by path, excluding the manifest itself. Run it
# from a clean tree right before committing a release (or after merging), in its own commit.
#
#   scripts/update_source_manifest.sh           rewrite SOURCE_MANIFEST.sha256
#   scripts/update_source_manifest.sh --check   exit 1 (and show the diff) if it is stale
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
manifest="$repo_dir/SOURCE_MANIFEST.sha256"
temporary="$manifest.tmp"

check=0
case "${1-}" in
    "") ;;
    --check) check=1 ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

cd "$repo_dir"
git ls-files --cached --others --exclude-standard | \
    LC_ALL=C sort | \
    while IFS= read -r path; do
        if [ "$path" != "SOURCE_MANIFEST.sha256" ] && [ "$path" != "SOURCE_MANIFEST.sha256.tmp" ]; then
            sha256sum -- "$path"
        fi
    done > "$temporary"
if [ "$check" -eq 1 ]; then
    if cmp -s "$temporary" "$manifest"; then
        rm -f -- "$temporary"
        echo "SOURCE_MANIFEST.sha256 is up to date"
        exit 0
    fi
    diff -- "$manifest" "$temporary" || true
    rm -f -- "$temporary"
    echo "SOURCE_MANIFEST.sha256 is stale: run scripts/update_source_manifest.sh" >&2
    exit 1
fi
mv -- "$temporary" "$manifest"
