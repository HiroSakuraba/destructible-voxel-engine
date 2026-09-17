#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
manifest="$repo_dir/SOURCE_MANIFEST.sha256"
temporary="$manifest.tmp"

cd "$repo_dir"
git ls-files --cached --others --exclude-standard | \
    LC_ALL=C sort | \
    while IFS= read -r path; do
        if [ "$path" != "SOURCE_MANIFEST.sha256" ]; then
            sha256sum -- "$path"
        fi
    done > "$temporary"
mv -- "$temporary" "$manifest"
