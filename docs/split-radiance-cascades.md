# Experimental split radiance cascades

The Vulkan player has an opt-in split-radiance-cascades prototype for diffuse global illumination. It is separate from the existing screen-space radiance cascades and is not a complete implementation of [Split Radiance Cascades](https://arxiv.org/abs/2607.20384).

Set `DVE_SPLIT_RC_EXPERIMENT=1` and run the player with `--renderer vulkan --gi cascades`. The existing renderer remains the default. For diagnostics, set `DVE_SPLIT_RC_PROFILE=1`; this prints per-stage CPU wall-clock timings, including synchronization and uploads, not isolated GPU timestamps. Startup compiles Vulkan pipelines and is much slower than warm frames.

The prototype reconstructs one visible depth sample per 8x8 pixels, inserts four levels of sparse world-space probe keys, traces 32 directions per valid sample through the voxel scene grid, splits hit radiance and transmittance into intervals, and composes the intervals far to near. Directional samples are stratified across nested bins. The player merge weights neighboring parent probes by their visible-surface sample count and averages available angular bins within each parent. A compute pass upsamples the result into the player indirect-light texture. Probe assignment and subsequent resource construction still include synchronous readbacks and per-frame work.

The prototype uses a bounded hash key and atlas, a coarse scene voxel grid and one nearest probe per sample per level. It is missing the paper's full hierarchical ray allocator, distance-based probe LODs, persistent world-space temporal history and multi-bounce cache. Sparse interpolation can leak light, and secondary hits use voxel palette values rather than full polygon textures or glass transmission. It is not a path-traced reference or a caustics renderer.

## Sponza fixture

Sponza is not included in this repository. For the Khronos glTF version supplied separately, build `dve_player`, `dve_cook_mesh` and `dve_cook_model`, then on Windows run:

```powershell
./scripts/prepare_sponza_test.ps1 -Model 'path/to/Sponza/glTF/Sponza.gltf' -Project 'path/to/new/sponza-test' -BuildDirectory 'path/to/build/Release' -DependencyBin 'path/to/vcpkg/installed/x64-windows/bin'
```

`-DependencyBin` is optional if the import tools' runtime libraries are already on PATH. The script refuses to overwrite an existing game project. It cooks a textured polygon mesh for display and a 0.25 m surface-voxelized copy for the GI tracer, and writes `Run Sponza Lighting Test.bat` in the new project. The separately supplied model and textures remain subject to their original license; no third-party Sponza assets are added to this repository.

The launcher sets `DVE_SPLIT_RC_PROXY_NAME=SponzaGIProxy`: in experimental split mode only, the matching voxel object is omitted from rasterization but remains in the voxel grid. Press G to compare split GI with the ambient fallback, Q/E to rotate the sun, Z/X to change its height, and R to reset it. Use Alt+mouse to orbit, pan or dolly and WASD to move. The voxel proxy is deliberately approximate and may cause false occlusion or miss thin features.

## Validation

The `dve_gpu_split_rc_*` tests exercise interval splitting, sparse probe assignment, voxel tracing, accumulation, angular sampling, weighted interpolation and multi-level merging. `dve_split_rc_player_tests` additionally checks repeated frames, emissive changes and removal, hidden-proxy light, GI toggling, camera movement, voxel destruction and resizing. Set `DVE_REQUIRE_VULKAN=1` and `DVE_VULKAN_VALIDATION=1` to require a Vulkan device and validation. The tests are not a substitute for comparing dynamic scenes against a path-traced reference.
