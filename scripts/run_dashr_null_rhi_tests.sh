#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/dve-dashr-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

common_sources="
    $repo_dir/src/render/dashr_live_surface.cpp
    $repo_dir/src/render/dashr_shell.cpp
    $repo_dir/src/render/dashr_atlas.cpp
    $repo_dir/src/dashr_surface.cpp
    $repo_dir/src/dashr_seam_map.cpp
    $repo_dir/src/rhi/device.cpp
    $repo_dir/src/rhi/null_device.cpp
    $repo_dir/src/polygon_asset.cpp
    $repo_dir/src/matrix4.cpp
    $repo_dir/src/master_material.cpp
    $repo_dir/src/material_parameter_collection.cpp
    $repo_dir/src/material_mapping.cpp
"

# Paths in this repository contain no shell whitespace. Word splitting expands
# this fixed source list into separate compiler arguments.
# shellcheck disable=SC2086
c++ -std=c++23 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$repo_dir/include" "$repo_dir/tests/test_dashr_live_surface.cpp" \
    $common_sources -o "$build_dir/dashr_live_surface"
"$build_dir/dashr_live_surface"

# shellcheck disable=SC2086
c++ -std=c++23 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$repo_dir/include" "$repo_dir/tests/test_live_environment_renderer.cpp" \
    "$repo_dir/src/render/live_environment_renderer.cpp" \
    "$repo_dir/src/render/mesh_rhi_mirror.cpp" \
    "$repo_dir/src/render/shadow_material_table.cpp" \
    "$repo_dir/src/render/main_material_table.cpp" \
    "$repo_dir/src/render/material_resource_residency.cpp" \
    "$repo_dir/src/render/environment_lighting_gpu.cpp" \
    "$repo_dir/src/render/cascaded_shadow_atlas.cpp" \
    "$repo_dir/src/render/cascaded_shadow_map.cpp" \
    "$repo_dir/src/gpu_material_buffer.cpp" \
    "$repo_dir/src/gpu_material_mapping.cpp" \
    "$repo_dir/src/environment_lighting.cpp" \
    "$repo_dir/src/environment_lighting_asset.cpp" \
    "$repo_dir/src/camera_system.cpp" \
    $common_sources -o "$build_dir/live_environment_renderer"
"$build_dir/live_environment_renderer"

python3 "$repo_dir/tools/validate_shader_contracts.py"
python3 "$repo_dir/scripts/compile_shaders.py" --backend spirv --dry-run >/dev/null
c++ -std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
    -fsyntax-only -I"$repo_dir/include" "$repo_dir/tests/test_vulkan_dashr.cpp"
