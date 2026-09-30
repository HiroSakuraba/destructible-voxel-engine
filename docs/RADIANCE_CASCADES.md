# Radiance Cascades — Phase 1: CPU 2D reference over voxel slices

Status: **Phase 1 (CPU flatland reference) landed.** Nothing here runs on the GPU or changes the
production lighting path (`GlobalIlluminationMode`, `voxel_lighting_plan`, the HLSL GI passes).

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

### Open questions for Phase 2

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
