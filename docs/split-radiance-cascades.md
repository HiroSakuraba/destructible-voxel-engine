# Experimental split radiance cascades

The Vulkan player has an opt-in split-radiance-cascades prototype for diffuse global illumination. It is separate from the existing screen-space radiance cascades and is not a complete implementation of [Split Radiance Cascades](https://arxiv.org/abs/2607.20384).

Set `DVE_SPLIT_RC_EXPERIMENT=1` and run the player with `--renderer vulkan --gi cascades`. The existing renderer remains the default. For diagnostics, set `DVE_SPLIT_RC_PROFILE=1`; this prints per-stage CPU wall-clock timings, including synchronization and uploads, not isolated GPU timestamps. Startup compiles Vulkan pipelines and is much slower than warm frames.

The prototype reconstructs one visible depth sample per 8x8 pixels, inserts four levels of sparse world-space probe keys, traces 32 directions per valid sample through the voxel scene grid, splits hit radiance and transmittance into intervals, and composes the intervals far to near. Directional samples are stratified across nested bins. The player merge weights neighboring parent probes by their visible-surface sample count and averages available angular bins within each parent. A compute pass upsamples the result into the player indirect-light texture. Probe assignment and subsequent resource construction still include synchronous readbacks and per-frame work.

The prototype uses a bounded hash key and atlas, a coarse scene voxel grid and one nearest probe per sample per level. It is missing the paper's full hierarchical ray allocator, distance-based probe LODs, persistent world-space temporal history and multi-bounce cache. Sparse interpolation can leak light, and secondary hits use voxel palette values rather than full polygon textures or glass transmission. It is not a path-traced reference or a caustics renderer.

## Follow-up: performance, temporal reuse, and GI ownership

This remains an **experimental** dynamic diffuse-GI mode. The follow-up removes
one avoidable GPU-to-CPU round trip by computing the world-space probe hash
and assignments on the CPU. The implementation matches the level/sheet/cell
packing of `split_rc_probe_assign_cs.hlsl`, including negative cells. It retains
the original GPU assignment path for explicit comparison:

```text
DVE_SPLIT_RC_EXPERIMENT=1
DVE_SPLIT_RC_PROFILE=1
DVE_SPLIT_RC_GPU_ASSIGN=1  (optional: re-enable the GPU diagnostic assignment)
```

World-space *probe layout* keys are retained while the candidate positions,
levels and surface sheets remain identical. This is not persistent radiance
history during a moving-camera sequence.

A separate **strict static-frame GI cache** reuses the previously resolved
full-resolution indirect-light texture, eliminating the entire experimental
trace/merge/readback sequence for an exact repeat frame. It requires unchanged
camera/view-projection, grid content generation, sky and sun parameters, and
diffuse GI settings. Camera movement, voxel destruction, in-place emissive
edits, resize and render-mode changes invalidate it. It excludes polygon
scenes, hidden proxy overrides, and explicitly requested surface diagnostics.
The player exposes the actual experimental trace count in
`last_stats().tracedRays`; zero on a valid static reuse.

**Limitations:** A moving camera still reads the coarse depth-derived
surfaces back to the CPU each frame; the diagnostic counters also require
readback. Most compute stages still submit and synchronize separately. These
remain architectural optimization tasks; no 60 fps claim is made. In particular,
Sponza's polygon draw path does **not** use the static reuse fast path.

The Vulkan player now selects one exclusive runtime *diffuse* indirect source:
environment IBL, traced one-bounce, screen-space cascades, or experimental split
cascades. In the material shader, traced indirect diffuse replaces environment
diffuse only at valid pixels; it is not added a second time. AO is applied
to environment diffuse but not re-applied to already occluded traced diffuse.
SHE/split-sum are alternative **specular** representations; direct sun and
clear coat are owned separately. An experimental split flag cannot hijack
one-bounce lighting.

**Acceptance:**
- Build `dve_split_rc_probe_layout_tests`,
  `dve_indirect_lighting_ownership_tests`,
  `dve_split_rc_player_tests`, and the existing split-RC Vulkan fixtures.
- Repeat the same voxel frame: first frame traces, next frame must report
  zero newly traced rays and produce the same output.
- Change emissive intensity in-place, remove voxels, move camera and resize:
  cache must invalidate and the lighting must update.
- Toggle GI modes while `DVE_SPLIT_RC_EXPERIMENT=1`: diffuse ownership must
  remain correct and the old indirect texture must not leak into another mode.
- Compare `DVE_SPLIT_RC_GPU_ASSIGN=1` with the new CPU assignment path for
  visual output, timing and memory on NVIDIA hardware.
- Measure 1080p GPU pass timings and CPU fence/readback wait separately;
  `DVE_SPLIT_RC_PROFILE` is CPU wall time, not isolated shader duration.

## Receiver-aware upsampling

The full-resolution GI reconstruction now evaluates the **actual receiver** from the current
depth buffer at each pixel instead of reusing the centre of its 8x8 ray tile. It rejects sky,
incompatible surface normals (dot product below 0.85), and candidates whose tangent planes
disagree by more than the existing voxel-size-scaled upsample tolerance (checked in both
directions). Compatible candidates are bilinearly normalized; pixels without valid support
retain alpha 0 and fall back to environment lighting rather than borrowing GI across a wall.

The regression fixture `dve_gpu_split_rc_screen_tests` builds a depth discontinuity *inside
one 8x8 tile* and verifies that the near/far plane and sky pixels do not share the coarse
sample's indirect radiance. This does not fix world-space probe visibility or light leaks
between coplanar but separated surfaces, and a very thin triangle with no valid neighbour
can still lose indirect light. The per-pixel position/normal reconstruction adds depth
reads and ALU work; record the isolated upsample GPU duration at 1080p on GTX 1080 before
claiming a performance result. The original split-RC mode remains opt-in and experimental.

## Sponza fixture

Sponza is not included in this repository. For the Khronos glTF version supplied separately, build `dve_player`, `dve_cook_mesh` and `dve_cook_model`, then on Windows run:

```powershell
./scripts/prepare_sponza_test.ps1 -Model 'path/to/Sponza/glTF/Sponza.gltf' -Project 'path/to/new/sponza-test' -BuildDirectory 'path/to/build/Release' -DependencyBin 'path/to/vcpkg/installed/x64-windows/bin'
```

`-DependencyBin` is optional if the import tools' runtime libraries are already on PATH. The script refuses to overwrite an existing game project. It cooks a textured polygon mesh for display and a 0.25 m surface-voxelized copy for the GI tracer, and writes `Run Sponza Lighting Test.bat` in the new project. The separately supplied model and textures remain subject to their original license; no third-party Sponza assets are added to this repository.

The launcher sets `DVE_SPLIT_RC_PROXY_NAME=SponzaGIProxy`: in experimental split mode only, the matching voxel object is omitted from rasterization but remains in the voxel grid. Press G to compare split GI with the ambient fallback, Q/E to rotate the sun, Z/X to change its height, and R to reset it. Use Alt+mouse to orbit, pan or dolly and WASD to move. The voxel proxy is deliberately approximate and may cause false occlusion or miss thin features.

## Validation

The `dve_gpu_split_rc_*` tests exercise interval splitting, sparse probe assignment, voxel tracing, accumulation, angular sampling, weighted interpolation and multi-level merging. `dve_split_rc_player_tests` additionally checks repeated frames, emissive changes and removal, hidden-proxy light, GI toggling, camera movement, voxel destruction and resizing. Set `DVE_REQUIRE_VULKAN=1` and `DVE_VULKAN_VALIDATION=1` to require a Vulkan device and validation. The tests are not a substitute for comparing dynamic scenes against a path-traced reference.
