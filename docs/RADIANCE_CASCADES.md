# Radiance Cascades

Status: **Phase 1 (CPU 2D flatland reference) and Phase 2 (CPU SPWI on the reference voxel
renderer) landed.** Nothing runs on the GPU yet. Phase 2 adds
`GlobalIlluminationMode::RadianceCascades` (value 3). Only `ReferenceVoxelRenderer` implements it;
GPU packing and `voxel_lighting_plan` treat it as `VoxelOneBounce`. §7 is the Phase 3 GPU
hand-off.

- Phase 1: §1–§4, CPU 2D reference over voxel slices.
- Phase 2: §5–§6, screen probes with world-space intervals.
- Phase 3: §7, GPU hand-off.

# Phase 1: CPU 2D reference over voxel slices

| Piece | Location |
|---|---|
| Module | `include/dve/render/radiance_cascades_2d.hpp`, `src/render/radiance_cascades_2d.cpp` (in `dve_core`) |
| Tests | `tests/test_radiance_cascades_2d.cpp` → ctest `dve_rc2d_tests` |
| Bench | `apps/rc2d_bench.cpp` → `dve_rc2d_bench [--out DIR] [--size N] [--bf-rays N] [--threads N] [--scale N]` |
| CI | not wired yet: the existing workflow only builds `dve_v235_foundations_tests`. A core-only `rc2d-cpu-reference` job is proposed in the Phase 1 PR description (the push token lacked the `workflow` scope). |

## 1. What the module does

- **Input.** A `Grid2D` of cells with occupancy, emission and albedo. You can build one by
  slicing voxel data at a plane (`slice_voxel_object`, `slice_packed_brickmap`, or
  `slice_voxels` with any `MaterialId(Int3)` callback) plus a `SliceMaterial` table indexed
  by `MaterialId`. `make_synthetic_scene` builds test scenes: `Empty`, `SingleLight`,
  `OccluderShadow`, `ThinWall` and `Room`. One slice cell is one voxel (0.1 m by default).
- **Radiance model.** Radiance cascades (RC) and the brute-force reference share it:
  - Opaque cells are emitters. Emission can be zero, which makes a black occluder.
  - A ray that leaves the grid sees `sky`.
  - Albedo is carried along for Phase 2 but not used yet, so this is emitter-only transport
    with no bounce.
  - The output is **fluence**: mean incoming radiance over all 2D directions at each
    non-opaque cell centre. Opaque cells output their emission.
- **Cascades.**
  - Cascade `i` places probes every `baseProbeSpacing·2^i` cells and gives each one
    `baseRayCount·rayBranching^i` directions. The defaults are 1, 4 and 4, following
    Yaazarai (GM Shaders), McGhee and Sannikov's Radiant 2d, so every level stores the same
    number of values.
  - The interval for cascade `i` is `[L0·(g^i−1)/(g−1), +L0·g^i)` with `L0 = 1` cell and
    `g = 4`.
  - The top level's interval is always stretched past the grid diagonal. That makes extra
    cascades a no-op instead of a sky leak.
  - `intervalOverlap` is optional. It extends every interval end by
    `overlap·√2·spacing_{i+1}`, which is the GM Shaders part 2 light-leak fix.
- **Tracing.** Each interval is traced with an exact Amanatides–Woo DDA over the grid
  (`trace_segment`). There is no SDF, so thin walls cannot be stepped over.
- **Merge.** Merging runs top-down with an RGB + transmittance (visibility) term:
  `near + near.t·far`. Level `i+1` is pre-averaged into level-`i` directions (GM Shaders
  part 2), so a vanilla merge takes 4 lookups.
  - **Vanilla:** one ray per interval, merged with the bilinearly interpolated, pre-averaged
    upper cascade.
  - **Bilinear fix:** each interval is traced from its usual start to the start of the child
    interval of *each* of the 4 bilinear upper probes. Each result is merged with that probe
    and the 4 are blended with bilinear weights. This follows Osborne & Sannikov 2024 §2.5 and
    MytinoDev's shadertoy, and costs 4× the rays.
  - **Parallax fix (experimental, my interpretation):**
    - One ray per interval. Each upper probe is sampled in the direction from that probe to
      the point the lower ray reaches at the middle of the upper interval, using a cone-width
      angular box filter over the un-averaged upper directions. No extra rays.
    - This is my reading of radiance.wiki's description of mxacop's fix ("reprojects sampling
      rays to the bilinearly sampled probes' positions"). I couldn't read the original
      shadertoy, which is behind Cloudflare. See Findings: it does not reproduce the claimed
      benefit.
- **Brute-force reference** (`solve_brute_force`). N stratified rays per cell (default 2048),
  rotated per cell by a deterministic hash and traced to the grid edge.
- **Metrics** (`compare_fluence`). Luminance RMSE and max absolute error over non-opaque
  cells, both absolute and relative to the mean reference fluence.
- **Determinism.** Rows are split statically across `std::thread`s (up to 16); each output is
  written by exactly one thread. Results are bit-identical for any thread count, and the
  tests check this.
- **Penumbra calculator port** (`penumbra_cascade`). Ports the default mode of Kornel's
  calculator (https://kornel.ski/radiance, 2024.08). The test values come from running the
  calculator's own `Cascades` class under Node, and the port matches it bit-for-bit, including
  the `tan(45°)=0.9999999999999999` quirk that counts an 8-cell interval as 9 steps.
  `check_penumbra_condition` applies the same criteria to our levels.

## 2. Results

Numbers from `dve_rc2d_bench --size 256 --bf-rays 2048` on the shared box: 8-core Intel Xeon,
no GPU, GCC 14 Release. "Auto" means 8 threads. Relative errors are luminance over non-opaque
cells divided by the mean brute-force fluence. The bench writes the full table, including the
`+overlap` rows, to `results.md` next to the PNGs.

### Default config (spacing 1, 4 base rays, 4×/2× branching, L0 = 1, 6 cascades)

| scene | mode | rays | ms (8 thr) | ms (1 thr) | rel RMSE | rel max | peak cascade MB |
|---|---|---:|---:|---:|---:|---:|---:|
| room | brute force (2048/px) | 128.9 M | 5403 | – | ref | ref | – |
| room | vanilla | 1.57 M | 41 | 106 | 0.332 | 31.0 | 5.0 |
| room | bilinear fix | 5.51 M | 96 | 389 | **0.170** | **10.5** | 5.0 |
| room | parallax fix | 1.57 M | 64 | 300 | 0.334 | 31.0 | 8.0 |
| occluder | vanilla | 1.57 M | 38 | 112 | 0.348 | 21.9 | 5.0 |
| occluder | bilinear fix | 5.51 M | 118 | 445 | **0.186** | **10.2** | 5.0 |
| occluder | parallax fix | 1.57 M | 72 | 327 | 0.348 | 21.9 | 8.0 |
| single light (r = 1.5) | vanilla | 1.57 M | 38 | 112 | 0.354 | 31.5 | 5.0 |
| single light (r = 1.5) | bilinear fix | 5.51 M | 116 | 449 | **0.255** | **18.8** | 5.0 |
| single light (r = 1.5) | parallax fix | 1.57 M | 71 | 329 | 0.369 | 32.3 | 8.0 |
| thin wall | vanilla | 1.57 M | 36 | 102 | 0.375 | 23.4 | 5.0 |
| thin wall | bilinear fix | 5.51 M | 100 | 406 | **0.195** | **10.9** | 5.0 |
| thin wall | parallax fix | 1.57 M | 68 | 317 | 0.375 | 23.5 | 8.0 |

Brute force takes 5.2–6.1 s per scene, so RC is about 50–160× faster at these settings. The
largest errors sit in the first few cells next to emitters, where cascade 0 has only 4
directions. That is why "rel max" is large even when RMSE is moderate.

### Thin-wall leak (1-voxel wall, light on the left, sky = 0; brute force is exactly 0 on the right)

| mode | right-side mean / left-side mean | right-side max |
|---|---:|---:|
| vanilla | 0.0144 | 0.0140 |
| vanilla + overlap | 0 | 0 |
| bilinear fix | 0 | 0 |
| parallax fix | 0.0116 | 0.0133 |
| parallax fix + overlap | 0 | 0 |

### Parameter sweep (room, 256², rel RMSE)

| mode | spacing | base rays | L0 = 0.5 | L0 = 1 | L0 = 2 | L0 = 4 |
|---|---:|---:|---:|---:|---:|---:|
| vanilla | 1 | 4 | 0.271 | 0.332 | 0.304 | 0.502 |
| vanilla | 1 | 16 | 0.277 | 0.235 | 0.140 | 0.138 |
| vanilla | 2 | 4 | 0.717 | 0.617 | 0.514 | 0.547 |
| bilinear fix | 1 | 4 | 0.232 | **0.170** | 0.216 | 0.320 |
| bilinear fix | 1 | 16 | 0.218 | 0.127 | **0.065** (308 ms, 20 MB) | 0.075 |
| bilinear fix | 2 | 4 | 0.701 | 0.527 | 0.389 | 0.478 |

### Destruction (VoxelObject → slice → solve, bilinear fix, 256²)

A 3D sphere blast (r = 10 voxels, 154 voxels removed via `set_voxel`) opens the interior wall.

| step | slice | solve | `set_voxel` blast |
|---|---:|---:|---:|
| before | 2.3 ms | 97 ms | – |
| after | 2.4 ms | 110 ms | 0.17 ms |

The warm light is the only source of red, so red fluence in the right room isolates it. It
goes from **0.000000** to **0.001297** on the first solve after the edit. There is no
precompute, cache or history to invalidate. After the edit, RC vs brute force has rel RMSE
0.120.

### Images (bench output, `/workspace/rc-shots/`)

- `<scene>_montage.png` for room, occluder, single-light and thin-wall. The top row is brute
  force, vanilla, bilinear fix and parallax fix. The bottom row is the scene, then error
  heatmaps (|ΔL| / (0.5·mean ref), black → red → yellow → white).
- Individual tiles: `<scene>_{scene,bruteforce,rc_*,err_*}.png`.
- `destruction_montage.png` plus `destruction_{before,after}_*.png`.

## 3. Findings

1. **Ringing is real and sits at cascade boundaries.** On a single disc light (r = 3, 128²),
   vanilla overshoots brute force by up to **43%** on the +x profile. The bumps line up with
   interval starts: +30% near d ≈ 18 at 128² (cascades 2/3, start 21) and +47% near d ≈ 82 at
   256² (cascades 3/4, start 85). The bilinear fix cuts the worst overshoot to **15%**. This
   matches Osborne & Sannikov 2024 §2.5 (errors up to ~10% in their harder setup) and McGhee's
   observation.
2. **The bilinear fix wins on every scene.** It roughly halves RMSE (0.33–0.37 → 0.17–0.20)
   and max error, and removes both the thin-wall leak and the umbra leak blobs visible in
   `occluder_montage.png`. It costs 3.5× the rays (the top level doesn't need the fix) and 2.3–3.1× the time with
   8 threads, or 3.7–4.0× with 1 thread. Memory is unchanged.
3. **The interval-overlap fix closes leaks but hurts accuracy with 4 base rays.** Rel RMSE goes
   0.33 → 0.63 on room and 0.35 → 0.85 on single light. It stretches cascade 0's 4 directions
   out to ~3.8 cells, which adds strong ray artifacts. It has no effect on the bilinear fix,
   which traces to the upper probes instead of the interval end.
4. **Parallax fix, my interpretation: not better than vanilla.** RMSE is the same, it still
   leaks, and it adds high-frequency angular noise (see the heatmaps), at 1.6–1.9× vanilla time
   and 1.6× memory, because it keeps un-averaged upper directions. Either the idea is weaker
   than the bilinear fix in this layout or I've reconstructed it wrongly. I'm treating it as
   unvalidated until the original shadertoy (https://www.shadertoy.com/view/XcfyDj) can be
   read.
5. **The default layout violates the penumbra condition for small lights.** By the
   calculator's criteria, a 1-cell light at 90° needs 7 rays at cascade 0's interval end,
   against 4 configured. Cascades 1–5 need 32, 132, 535, 2143 and 8577 rays against 16, 64,
   256, 1024 and 4096. The r = 1.5 light shows the result: a kaleidoscope pattern and the
   worst RMSE. Raising base rays to 16 with L0 = 2 gives the best accuracy found:
   **0.065 rel RMSE with the bilinear fix**, about 4× the cost of the default.
6. **Probe spacing 2 is poor near geometry.** Rel RMSE is 0.39–0.72 because the final
   bilinear gather mixes in probes that sit inside walls and lights. SPWI (Phase 2) places
   probes on surfaces, which avoids this.
7. **Memory and scaling behave as advertised.** Every level holds 4·W·H values (65 536 at 128²;
   the tests check this), so the whole hierarchy is (levels)·M0, and a CPU solve keeps only two
   levels resident: 5 MB peak at 256² (8 MB for the parallax fix). Cost is independent of the
   number of lights (Room has 3; Single light has 1; timings are the same).
8. **Destruction needs no special handling.** Edit, re-slice (about 2 ms at 256²) and re-solve
   gives correct light through the new opening on the first frame. Voxel-DDA intervals are
   exact, so there are no SDF-epsilon misses through 1-voxel walls; the only leaks come from
   merge interpolation, which is point 2.

## 4. How this feeds Phase 2 (CPU SPWI: screen probes, world-space intervals)

- **Use the bilinear fix as the baseline merge.** It is the only mode here that is both leak-
  free and clearly more accurate. Budget for 4× interval rays below the top cascade, or plan an
  explicit comparison against a cheaper variant (Yaazarai's nearest/interlaced fixes).
- **Size cascade 0 against the penumbra condition, not the 4-ray default.** In 3D, a direction
  count per probe of 4×4 or 6×6 on the octahedral map at cascade 0 is the analogue of the 16
  base rays that won here. `check_penumbra_condition` is the tool; a 3D version is needed.
- **Keep `trace_segment` semantics for 3D.** Exact voxel DDA per interval, first opaque voxel
  at `t < length`, and grid exit meaning sky. That maps directly onto `TraceVoxelRay`
  (`shaders/common/brick_trace_common.hlsli`), which already takes `maxDistance`; it only
  needs an origin offset for the interval start.
- **Certify against brute force the same way.** Phase 2 should reuse `compare_fluence`-style
  metrics against `ReferenceVoxelRenderer::global_illumination` with far more than its
  16-sample cap. The Phase 1 thresholds (rel RMSE < 0.23 for the bilinear fix on room at 128²)
  give a sense of what "correct RC" looks like.
- **Destruction:** no RC-specific invalidation is needed. Temporal reuse is the only thing
  that will need `AppliedBrickEdit`-driven resets.

### Open questions for Phase 2 (answered in §6)

Question 1 was not pursued, because the bilinear fix is leak-free in 3D too. Questions 3 and
4 are answered by §6 findings 1–2 and §5.3. Question 2 shows up as the screen-edge fallback
rays.


1. Parallax fix: get mxacop's actual formulation. If it is a reprojection of the bilinear-fix
   rays, it may be subsumed by the bilinear fix.
2. The bilinear fix overshoots the far field by 10–17% near grid edges, where clamped upper
   probes are reused. Should edges get their own treatment? The paper notes the same edge issue
   (Osborne & Sannikov 2024 §2.5).
3. Interval growth: the calculator's penumbra-driven layout is ×2 angular / ×2 interval (paper
   eq. 19), while we use ×4/×4. Test the ×2 scheme on the same scenes before fixing the 3D
   layout.
4. SPWI probes sit on a depth buffer, so bilateral (depth/normal) weights replace the grid
   bilinear weights. How the bilinear fix generalises there is not covered by any source I read.

# Phase 2: CPU SPWI on the reference voxel renderer

| Piece | Location |
|---|---|
| Module | `include/dve/render/radiance_cascades_spwi.hpp`, `src/render/radiance_cascades_spwi.cpp` (namespace `dve::render::spwi`, in `dve_render_bridge`) |
| Renderer mode | `GlobalIlluminationMode::RadianceCascades` in `ReferenceVoxelRenderer::render`; settings in `ReferenceVoxelRenderer::radianceCascades` (`RadianceCascadeSettings`) |
| Tests | `tests/test_radiance_cascades_spwi.cpp` → ctest `dve_rc_spwi_tests` (2 threads, `PROCESSORS 2`, ~20 s) |
| Bench | `apps/rc_spwi_bench.cpp` → `dve_rc_spwi_bench [--out DIR] [--width W] [--height H] [--bf-samples N] [--threads N] [--sweep 0/1] [--large 0/1]` |
| Shots / tables | `/workspace/rc-shots/p2-*.png`, `p2-results.md`, `p2-sweep.csv` |

## 5. What Phase 2 does

### 5.1 Pre-fix: the CPU GI bounce now includes the sun, like `resolve_gi.hlsl`

Before this change, `ReferenceVoxelRenderer`'s `VoxelOneBounce` evaluated bounce hits as
`one_bounce_diffuse_radiance(..., sunVisibility = 0)`. That is ambient hemisphere plus emissive
only. The GPU path (`generate_gi_sun_rays.hlsl` + `resolve_gi.hlsl`) instead traces one hard
sun-visibility ray per bounce hit and adds `sun · max(0, n·l) · vis / π`. The CPU now does the
same through `spwi::bounce_hit_radiance`:

- The ray starts at `hit + n · max(1e-3, shadowBias)`.
- The ray is skipped when `n·l ≤ 0` or `sunIntensity = 0`. That gives the same result as
  always tracing it.
- Sun rays are counted in `RenderStats::globalIlluminationSunRays`, so `shadowRays` keeps
  meaning primary shadow rays.

A wall facing away from a straight-down sun used to get zero indirect under a black sky. It now
gets 0.334 (the floor's radiance is 0.764), and it drops to exactly 0 once a roof blocks the
sun (`test_bounce_includes_sun_like_gpu`). The brute-force reference, the MC16 baseline and
every cascade interval use this same hit model.

All secondary rays in `ReferenceVoxelRenderer` now go through `spwi::VoxelSceneTracer`:

- It keeps a dense per-instance snapshot of the allocated bricks, built once per `render()`.
- It has an exact early-out once the ray leaves the occupied bounds.
- Its hits are bit-identical to `raycast_voxels_transformed`, which follows the same DDA rules
  as `TraceVoxelRay`: first opaque voxel at `t < maxDistance`, and grid exit means a miss
  (checked on 4000 random rays, with and without the snapshot, including a rotated instance).

### 5.2 Layout

- **Probes** sit on the primary-hit G-buffer (the same depth test as the renderer). A probe at
  level *i* samples the pixel at the centre of each 2^(i+1) px block (`baseProbeSpacingPixels`
  = 2). It stores position, voxel-face normal and view distance, and the ray origin is offset
  by the shadow bias along the normal. Pixels without a hit (sky) make invalid probes.
- **Directions** use an octahedral map with +Y at the centre. The resolution is 8×8 at level 0
  and doubles per axis per level, so 64 → 256 → 1024 → 4096 texels. Only texels in the probe's
  hemisphere (`d·n > 0`) are traced.
  - The texel solid angles come from the exact octahedral Jacobian (dΩ = 4 du dv / |P|³) with
    sub-texel quadrature. They sum to 4π to within 0.4%.
  - Doubling the resolution per axis gives 4× the directions, which with 4× fewer probes keeps
    memory constant per level. This is the ×4/×4 layout from Phase 1 and GM Shaders.
- **Intervals** are in world units: level *i* covers [L₀·(4ⁱ − 1)/3, L₀·(4ⁱ⁺¹ − 1)/3) with
  L₀ = 1 voxel. The top level always ends at `globalIlluminationMaxDistanceMeters`.
  - Level count: add levels until the next start would pass the max distance, until 10, or
    until a level would have fewer than 2 probes on the long screen axis.
  - With the default max distance of 48 that gives 4 levels: [0,1) [1,5) [5,21) [21,48].
  - `intervalScaling = ProbeSpacing` scales each probe's intervals by its level-0 cell size at
    its depth, i.e. depth-scaled intervals.
- **Tracing** calls `VoxelSceneTracer::trace(start, dir, length)` per interval. A hit returns
  `bounce_hit_radiance` (albedo·(ambient + sun·vis/π)·(1 − metallic) + emissive) with
  transmittance 0. A miss returns radiance 0 with transmittance 1. The top level's misses see
  `environment_radiance` (the sky/ground lerp that `resolve_gi` uses).

### 5.3 Merge (top-down, two levels resident)

For each lower probe *p*, the four upper probes get weights from the bilinear footprint in
screen space multiplied by a bilateral term:
`w_k = bilinear_k · exp(−(|n_p·(x_k − x_p)| / σ)²) · max(0, n_p·n_k)^8`. Here
σ = `depthToleranceProbeSpacings` (1) × upper spacing × pixel footprint × view distance. Upper
directions are pre-averaged 2×2 by solid angle down to the lower level's resolution. At lower
texel centres this is exactly a bilinear lookup of the upper map. Per lower texel:

- **Vanilla:** trace [start, end + overlap) from *p*. If it misses, add Σ w̃_k · upper_k.
- **Bilinear fix, adapted to bilateral weights:** trace one ray from *p*'s interval start
  directly to each upper probe's own interval start (plus overlap), `x_k + d·(start_{i+1} + overlap)`,
  and continue with that probe's radiance. The results are blended by the normalised w̃_k. This
  is the 2D bilinear fix with the bilinear weights replaced by bilateral ones. Probes with
  zero weight get no ray.
- **Overlap** is `intervalOverlap` (1) × the upper probe spacing, converted to world units at
  the probe's depth. It extends each lower interval past the upper start, which closes the
  gaps between the lower and upper rays that let light through thin walls.
- **Fallback:** when no upper probe is compatible (a silhouette, an isolated surface, or the
  screen edge), one ray goes to the GI max distance.
- **Final gather:** each pixel takes a cosine-weighted integral of cascade 0 about its own
  normal, normalised by Σ cos·dΩ over valid texels. The weights are cached per distinct
  normal. The result is blended over the four nearest level-0 probes with the same bilateral
  weights, with a per-pixel fallback.
- The result goes into the existing shading as `indirect`, i.e. `base · indirect`, like
  `VoxelOneBounce`.

Everything is deterministic. The work is split into fixed contiguous ranges, so results are
bit-identical for 1, 2 and 3 threads.

### 5.4 Brute-force reference and metrics

`solve_brute_force_indirect` uses N cosine-distributed rays per pixel (a Hammersley set rotated
per pixel by a hash) to the GI max distance. It uses the same hit model and environment, and it
is deterministic. The bench uses N = 2048 and the tests use 512. That is 128× and 32× the
16-sample `VoxelOneBounce` cap. `compare_indirect` reports luminance RMSE, relative RMSE (÷
mean reference), relative max and mean bias over pixels with a hit.

On an open floor under a sky/ground gradient, the brute force matches the analytic cosine
average (t̄ = 5/6) to 0.02%, and RC matches it to 0.3%. Under a constant environment RC returns
the constant exactly.

### 5.5 Destruction

The whole solve is rebuilt every frame from the current voxels: the tracer snapshot (0.6–0.9
ms), the G-buffer, all cascades and the gather. There is **no temporal accumulation, so no
history to reset**. An edit shows up in the very next frame. `AppliedBrickEdit`-driven
invalidation is only needed once Phase 3 adds temporal reuse (see §7.4).

## 6. Phase 2 results

Numbers are at 320×180 unless noted, with brute force at 2048 spp and 8 threads. MC16 is a
16-spp estimator with the same hit model, i.e. the `VoxelOneBounce` cap. All numbers are from
`p2-results.md`.

### Accuracy and cost (default = bilinear fix + overlap, 8×8 dirs, 2 px spacing, ×4 intervals)

| scene | mode | rel RMSE | rel max | bias | ms | interval rays |
|---|---|---:|---:|---:|---:|---:|
| courtyard | MC16 | 0.073 | 0.41 | 0.000 | 24 | 0.21 M |
| courtyard | vanilla (no overlap) | 0.079 | 0.73 | −0.028 | 36 | 0.40 M |
| courtyard | vanilla + overlap | **0.044** | 0.40 | 0.000 | 35 | 0.40 M |
| courtyard | bilinear fix (no overlap) | 0.067 | 0.56 | 0.004 | 65 | 0.95 M |
| courtyard | **bilinear fix + overlap** | 0.054 | 0.50 | 0.013 | 80 | 0.95 M |
| courtyard | bilinear + overlap, ×2 intervals (paper layout) | 0.098 | 0.53 | 0.023 | 102 | 1.23 M |
| courtyard | bilinear + overlap, depth-scaled intervals | 0.066 | 0.39 | 0.017 | 83 | 1.06 M |
| courtyard | bilinear + overlap, 4×4 dirs | 0.081 | 0.53 | 0.017 | 19 | 0.21 M |
| bunker | MC16 | 0.044 | 0.23 | 0.000 | 57 | 0.92 M |
| bunker | vanilla + overlap | **0.011** | 0.14 | 0.004 | 66 | 1.81 M |
| bunker | **bilinear fix + overlap** | 0.012 | 0.13 | 0.005 | 192 | 5.50 M |
| bunker | bilinear + overlap, 4×4 dirs | 0.033 | 0.19 | 0.001 | 51 | 1.30 M |
| thin wall (emissive panel only) | MC16 | 0.815 | 5.19 | 0.001 | 21 | 0.30 M |
| thin wall | vanilla + overlap | 0.170 | 1.21 | 0.079 | 30 | 0.53 M |
| thin wall | **bilinear fix + overlap** | 0.160 | 0.93 | 0.069 | 78 | 1.51 M |

The default beats the 16-spp GI cap everywhere. In the scenes with the sun and sky (courtyard
and bunker) it is about 1.4× and 3.5× better. In the small-emitter thin-wall scene it is about
5× better, where MC16 is mostly noise. Its cost is about 3× MC16 at 320×180. RC error is
bias-like (smooth and structured, see the error maps) rather than noise, so it would not
average out temporally the way MC does.

### Thin-wall leak (1 voxel = 0.1 m wall, lamp on the left, black sky; brute force is exactly 0 on the right)

| mode | right mean / left mean | right max / left mean |
|---|---:|---:|
| vanilla (no overlap) | 0.0077 | 0.099 |
| vanilla + overlap | 0 | 0 |
| bilinear fix (± overlap) | 0 | 0 |
| bilinear + overlap, ×2 intervals | 0.00002 | 0.001 |

The tests check the same scene at 320×180:

- vanilla with ×2 intervals and no overlap leaks 5% (the sensitivity check)
- bilinear with ×2 intervals and no overlap stays below 5e-4
- vanilla + overlap and the default stay below 1e-4

The camera looks straight down, so floor probes on both sides of the wall are coplanar and the
bilateral weights cannot separate them. Only the merge geometry does.

At 128×72, vanilla + overlap leaks 1.4%, while the bilinear fix stays at 0. That is why the
bilinear fix is the default even though vanilla + overlap is cheaper and slightly more accurate
at 320×180.

### Cost versus resolution (courtyard)

| resolution | mode | ms (8 thr) | ms (1 thr) | rel RMSE | peak MB | all-levels MB |
|---|---|---:|---:|---:|---:|---:|
| 320×180 | MC16 | 22 | 66 | 0.073 | – | – |
| 320×180 | vanilla + overlap | 40 | 85 | 0.044 | 14.7 | 46.7 |
| 320×180 | **default** | 67 | 182 | 0.054 | 14.7 | 46.7 |
| 320×180 | default, 4×4 dirs | 20 | 45 | 0.081 | 3.7 | 11.7 |
| 640×360 | MC16 | 87 | 275 | 0.072 | – | – |
| 640×360 | vanilla + overlap | 138 | 314 | 0.031 | 57.4 | 183.8 |
| 640×360 | **default** | 283 | 803 | 0.034 | 57.4 | 183.8 |
| 640×360 | default, 4×4 dirs | 76 | 185 | 0.070 | 14.3 | 46.0 |

- Brute force at 2048 spp takes 2.7 s at 320×180 and 10.4 s at 640×360.
- Memory is 13 B per direction texel (RGB32F + a valid byte). The layout keeps it constant per
  level: 19,160 probes over 4 levels at 320×180.
- "Peak" counts one level plus the pre-averaged upper level, which is all the CPU keeps.
  "All-levels" counts every level kept, as a GPU would.
- The tracer snapshot is 25 KiB.
- RC error falls with resolution (0.054 → 0.034) because the probes get denser in world
  space. MC16 error does not.

### Destruction (bunker: 54 voxels of the 1-voxel wall removed between frames)

| frame | render ms (RC mode) | receiver RC | receiver brute force | rel RMSE (whole frame) |
|---|---:|---:|---:|---:|
| before | 227 | 0.0153 | 0.0153 | 0.012 |
| after | 247 | 0.0177 | 0.0178 | 0.111 |

- `remove_box` took 0.07 ms.
- "Receiver" is the mean indirect luminance on the room floor within 6 voxels of the wall.
- RC tracks brute force to within 1% on the very next frame.
- After the edit, the error concentrates on the 1-voxel jamb faces and the wall base beside the
  new sunlit patch (room-only rel RMSE 0.112).

### Parameter sweep (288 configs; `p2-sweep.csv`)

- **Directions matter most.** 8×8 at level 0 is far better than 4×4 (courtyard 0.054 vs
  0.081; thin wall 0.16 vs 0.79). 4×4 misses the small emitter.
- **×4 intervals are better than ×2** in every combination. ×2 needs 6 levels here and is both
  worse and slower.
- **Probe spacing 1 px** is the best overall (vanilla + overlap: courtyard 0.034, bunker 0.012),
  but it costs about 4× the rays and memory. Spacing 2 is the knee.
- **World intervals are equal to or slightly better than depth-scaled ones** at these scales.
- **Overlap 1 helps** every vanilla configuration and most bilinear ones.
- **L₀ = 1 voxel** is best. 0.5 and 2 are within a few percent.

### Images (`/workspace/rc-shots/`)

- `p2-<scene>-montage.png` for scene ∈ {courtyard, thin-wall, bunker}:
  - top row: final with brute-force GI, indirect brute force, MC16, vanilla, vanilla + overlap
  - bottom row: the default, then error maps for MC16, vanilla, vanilla + overlap and the
    default
  - error maps: |Δ luminance|, full scale = 0.5 × mean reference
- Per mode: `p2-<scene>-indirect-<mode>.png` and `p2-<scene>-err-<mode>.png`.
- Finals: `p2-<scene>-final-rc.png` and `p2-<scene>-final-bruteforce.png`.
- Destruction: `p2-destruction-{before,after}-{final,indirect}.png`,
  `p2-destruction-after-{bruteforce,err}.png` and `p2-destruction-montage.png`.

### Findings

1. **The bilinear fix only works when the intervals keep up with the probe spacing in world
   units.**
   - With ×2 intervals and 2 px spacing, the fix's rays from *p* to the four upper probes are
     strongly skewed. The upper probes are several voxels apart while the interval is 1–2
     voxels, so the far field is sampled at the wrong angle, giving a +2% bias and 0.098 rel
     RMSE.
   - ×4 intervals and 8×8 directions fix it. Depth-scaled intervals are the other option.
   - This is the 3D form of Phase 1's open question 4, and it answers open question 3 (×2 vs
     ×4) in favour of ×4 for SPWI.
2. **Vanilla + overlap is the cheapest good option** (2.4–3× fewer rays than the bilinear
   fix). It needs enough resolution: at 128×72 it leaks 1.4% through a 1-voxel wall.
   Vanilla also shows periodic blotches along wall bases (`p2-courtyard-err-vanilla.png`),
   the classic cascade-boundary ringing.
3. **The residual error is at contact corners and geometry narrower than a level-0 probe
   footprint.** Examples are the wall bases, 1-voxel jambs, and the floor next to walls. The
   bilateral weights reject the neighbours there and each pixel is left with one probe's
   hemisphere. A per-pixel final gather at cascade 0, or 1 px spacing near edges, would help.
4. **The octahedral map is hemisphere-wasteful.** Half of each probe's texels face into the
   surface and are never traced, but they are still stored. A hemi-octahedral map around the
   normal would halve memory (open question 3).
5. **Units:** the CPU reference renderer uses `globalIlluminationMaxDistanceMeters`,
   `shadowBias`, etc. directly as world units, where 1 voxel = 1 unit. The GPU packer converts
   metres to voxels with `metersPerVoxel` (0.1 in the engine). The RC interval lengths here are
   therefore in voxels. Phase 3 must pick one convention (open question 1).
6. **`voxel_lighting_plan` does not schedule the GI sun-ray passes.** `resolve_gi.hlsl` reads
   `gGiSunResults` (t9) and `generate_gi_sun_rays.hlsl` is in the shader manifest, but
   `make_voxel_lighting_frame_plan` emits only generate/trace/resolve GI. This was not changed
   here (no GPU wiring). Phase 3 should add the passes, or confirm the GPU host issues them
   elsewhere.

### Open questions

1. Units: should the CPU reference interpret the `*Meters` fields in metres (÷ metersPerVoxel)
   like the GPU? That would change `ReferenceVoxelRenderer` output for every GI/shadow
   distance, so it needs its own change and golden updates.
2. Default merge: keep the bilinear fix (robust against leaks at low resolution, about 2.4×
   the rays) or switch to vanilla + overlap (cheaper, and better at 320×180+)? A GPU could
   also pick per resolution.
3. A hemi-octahedral direction map around each probe's normal (halves storage and wasted
   texels) versus a world-aligned octahedral map (simpler merges across normals). The thesis
   uses a world-aligned map.
4. Temporal: RC's error is structured bias, not noise. Is temporal accumulation worth it at
   all in Phase 3, or only for jittered probe placement? If it is used, it needs the
   `AppliedBrickEdit` reset described in §7.4.

## 7. Phase 3 GPU hand-off

### 7.1 Passes (per frame, after the primary trace, replacing the GI trio when mode = 3)

| # | pass | dispatch | reads | writes |
|---|---|---|---|---|
| 1 | `BuildRadianceCascadeProbes` (one per level, or one pass over all levels) | probes_i | `gPrimaryResults`, `gPrimaryRays` (G-buffer: hit, voxel, normal, distance) | `gRcProbes[i]` (position, normal, view distance, valid) |
| 2 | `TraceRadianceCascadeIntervals` (per level, top → 0) | probes_i × dirs_i (hemisphere) | brick tables (`TraceVoxelRay` with origin = probe + d·start, `maxDistance` = interval, or up to 4 rays per texel for the bilinear fix), `gRcProbes[i]`, `gRcProbes[i+1]`, `gRcRadiance[i+1]` (pre-averaged) | `gRcHit[i]` (per ray: material, normal, hit position) |
| 3 | `TraceRadianceCascadeSun` (per level) | hits of pass 2 | brick tables | sun visibility per hit (same as `generate_gi_sun_rays`) |
| 4 | `MergeRadianceCascade` (per level) | probes_i × dirs_i | pass 2/3 results, bilateral weights to `gRcProbes[i+1]`, `gRcRadiance[i+1]` | `gRcRadiance[i]` |
| 5 | `PreAverageRadianceCascade` (per level > 0) | probes_i × dirs_i / 4 | `gRcRadiance[i]` | the pre-averaged map at level i − 1's resolution (or fold into pass 4 with a bilinear octahedral fetch) |
| 6 | `GatherRadianceCascades` | pixels | `gRcRadiance[0]`, `gRcProbes[0]`, the G-buffer | `gIndirectDiffuse` (u3), the same buffer `resolve_gi` writes |
| 7 | `ShadePrimary` | pixels | unchanged; it already reads `gIndirectDiffuse` (t11) | – |

- Passes 2–4 can be fused as trace → evaluate the hit material/sun inline → merge, if a
  second `TraceVoxelRay` for the sun inside the same thread is acceptable. The CPU does
  exactly that.
- The top level has no upper probes. It traces to the GI max distance and adds
  `EnvironmentRadiance` on a miss.

### 7.2 Buffers (1920×1080, RGBA16F = 8 B per direction texel, per level constant)

| configuration | probes L0 | texels per level | per level | ping-pong (2 levels) | all levels (5) |
|---|---:|---:|---:|---:|---:|
| 2 px, 8×8 (CPU default) | 960×540 | 33.2 M | 265 MB | 531 MB | ~1.3 GB |
| 4 px, 8×8 | 480×270 | 8.3 M | 66 MB | 133 MB | ~330 MB |
| 4 px, 4×4 (thesis) | 480×270 | 2.1 M | 17 MB | 33 MB | ~83 MB |

- The CPU default is **not** a GPU budget at 1080p. Start with 4 px / 4×4, then measure
  4 px / 8×8 and a hemi-octahedral 8×8 (half the texels).
- Keep only two levels resident. The top-down merge needs only level i + 1 (pre-averaged)
  while writing level i.
- Probe buffers are about 32 B per probe per level (float3 position, packed normal, view
  distance, flags).
- Bilinear-fix ray results do not need storing if pass 2 merges inline.

### 7.3 What `voxel_lighting_plan` needs wired

- New `VoxelLightingPass` entries, in this order: `BuildRadianceCascadeProbes`,
  `TraceRadianceCascadeIntervals`, `TraceRadianceCascadeSun`, `MergeRadianceCascade`,
  `GatherRadianceCascades`.
  - Emit one dispatch per level with its element count (probes_i × dirs_i, or the hemisphere
    count). `validate()` must still require `ShadePrimary` last.
- New capacities on `VoxelLightingFramePlan`: `radianceCascadeLevels`, per-level probe/texel
  counts (from a GPU port of `spwi::describe_cascades`), `radianceCascadeRadianceBytes` (two
  levels) and `radianceCascadeProbeBytes`.
  - Keep `globalIlluminationRayCapacity = 0` in mode 3 so the one-bounce buffers are not
    allocated.
- `pack_gpu_render_environment` should send 3 instead of the current VoxelOneBounce fallback
  (`kGlobalIlluminationRadianceCascades = 3u` is already reserved in
  `shaders/common/render_environment.hlsli`). `resolve_gi.hlsl` already zeroes
  `gIndirectDiffuse` for any mode ≠ 2, so skip it in mode 3 rather than let it clobber the
  gather.
- RC parameters in the constant buffer or `GpuRenderEnvironment`: spacing, base resolution,
  L₀, growth, overlap, the depth tolerance and the normal power. The layout is guarded by
  `tools/validate_shader_contracts.py`, so update both sides.
- Add the missing GI sun-ray passes for `VoxelOneBounce` (finding 6) while in there.
- Tests: extend `dve_voxel_lighting_plan_tests` (mode 3 dispatch list and capacities). The GPU
  output should be checked against `spwi::solve_radiance_cascades` on the synthetic scenes
  (`spwi::make_synthetic_scene`) with the thresholds in `dve_rc_spwi_tests`.

### 7.4 Destruction on the GPU

Without temporal reuse nothing needs invalidating: the brick tables are the only scene input,
and they are already updated by the edit path. If Phase 3 adds temporal accumulation of cascade
radiance, reset (or clamp the history weight of) every probe whose interval AABB intersects an
`AppliedBrickEdit` brick bound. At minimum, reset every probe within the GI max distance of the
edit. A global reset on any edit is the simple first version. The CPU destruction test is the
pass criterion: the receiver matches brute force within 5% on the first frame after the edit.

## Sources

- A. Sannikov, *Radiance Cascades: A Novel Approach to Calculating Global Illumination* (WIP) —
  https://github.com/Raikiri/RadianceCascadesPaper (penumbra condition §2.1, scaling eq. 19,
  memory §2.5, Radiant 2d §4.1, SPWI §4.5, limitations §5).
- C. Osborne & A. Sannikov, *Radiance Cascades: A Novel High-Resolution Formal Solution for
  Multidimensional Non-LTE Radiative Transfer* (2024) — https://arxiv.org/abs/2408.14425 (ringing
  and bilinear fix §2.5, Figs. 6–8).
- Yaazarai, GM Shaders *Radiance Cascades* parts 1 and 2 — https://mini.gmshaders.com/p/radiance-cascades,
  https://mini.gmshaders.com/p/radiance-cascades2 (4×/4× layout, pre-averaging, overlap leak fix).
- J. McGhee, *Building Real-Time Global Illumination: Radiance Cascades* — https://jason.today/rc
  (ringing in linear colour, merge clamping).
- MytinoDev, bilinear and forking fix — https://www.shadertoy.com/view/4clcWn; Yaazarai's bilinear /
  nearest / interlaced variants — https://github.com/Yaazarai/GMShaders-Radiance-Cascades.
- mxacop, parallax fix — https://www.shadertoy.com/view/XcfyDj; description at
  https://radiance.wiki/techniques/parallax-fix.
- Kornel, penumbra-condition calculator — https://kornel.ski/radiance.
- Overview and links — https://radiance-cascades.com/.
- A. Gavras, *Radiance Cascades with screen-space probes and world-space intervals* (Chalmers
  MSc thesis, 2025): https://odr.chalmers.se/items/3ee9fb4e-1880-46c2-802a-a660e38dc9ee. It
  covers SPWI, the bilateral "Bilinear 3D" merge, 16 rays at 4 px spacing, ×4 intervals, and
  the cosine-normalised gather. Code: https://github.com/Qirias/RC-SPWI.
- A. Sannikov, depth-aware upscaling for screen-space cascades (the thesis's ref. [6]):
  https://www.shadertoy.com/view/4XXSWS.
