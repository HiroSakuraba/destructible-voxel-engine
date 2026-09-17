#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
output_path=${TMPDIR:-/tmp}/dve_v235_foundations_tests
sanitize_flags=

if [ "${1:-}" = "--sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
fi

c++ -std=c++23 -O1 -g -Wall -Wextra -Wpedantic -Werror $sanitize_flags \
    -I"$repo_dir/include" \
    "$repo_dir/tests/test_v235_foundations.cpp" \
    "$repo_dir/src/v235_foundations.cpp" \
    "$repo_dir/src/navigation_mesh.cpp" \
    "$repo_dir/src/animation.cpp" \
    "$repo_dir/src/transform.cpp" \
    "$repo_dir/src/rigid_body_adapter.cpp" \
    -pthread -o "$output_path"

"$output_path"
