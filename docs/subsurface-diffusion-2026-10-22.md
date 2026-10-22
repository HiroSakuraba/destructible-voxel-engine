# Subsurface, part 3: translucency from the ground truth, and Burley diffusion (22 Oct 2026)

Builds on part 2 (the random walk). Part 2 showed that part 1's translucency formula (Burley's
thickness form, as in Unity HDRP and Unreal) is far from the truth, and that Burley's reflectance
profile is close. So:
- translucency now uses the walk's own numbers;
- front-lit light spreads with Burley's profile.

## Translucency from a baked table (`include/dve/render/subsurface_transmission.hpp`)

`apps/dve_bake_subsurface_transmission` runs the random walk through slabs and writes:
- `src/render/subsurface_transmission_table.inc` (CPU);
- `shaders/common/subsurface_transmission_table.hlsli` (GPU).

**Each entry:** of the light arriving at cosine μ on one face of a slab t / d scatter distances
thick, the share that comes out the other face after scattering at least once.
- The medium is Chiang's for albedo A: isotropic and index-matched.
- Light that crosses without scattering is left out. It is a straight beam (the sun seen through
  the slab), not a glow, and Subsurface materials draw opaque.

**Grid:**
- μ = 0.2 to 1 in steps of 0.1;
- A = 0 to 1 in steps of 0.05;
- t / d = 16 (k / 32)² for k = 0 to 32, dense where the curve bends near thin slabs.

That is 6237 entries, 300 000 walks each, about 7 minutes on two cores. Lookups are trilinear,
with an exponential continuation past t / d = 16.

**Shaded-side light** = sun × cos at the sunlit face × that face's visibility × T(A, t / d, μ), per
channel with the subsurface colour as A. The GPU shader reads the same table.

What changes on screen:
- **Thin sheets glow less than before.** At t = 0.5 d, most light goes straight through. The glow
  peaks around t ≈ 1–2 d.
- **Thick objects glow more and farther:** at 8 d, about 3× part 1.
- **Brightest glow:** set the scatter distance near the thickness.

## Burley diffusion of front-lit light

On Subsurface materials, the sun's term (cosine × shadow) at a point is replaced by its average over
the surface around it, weighted by Burley's profile R(r) ∝ (e^(−r/d) + e^(−r/3d)) / r.

**How it is sampled:**
- Sample radii follow the profile exactly: it is a ¼ / ¾ mixture of exponentials with means d and
  3d, and samples are stratified. So the weighted average is a plain mean.
- Each disc point is put onto the surface by a probe ray along the normal, which must land on the
  same instance and material. Points with no such surface are left out and the rest renormalised,
  so edges do not darken.
- Each sample takes one sun ray, across the sun disc in soft-shadow modes.

**Effect:** light bleeds into shadow edges and past the terminator. A flat, fully lit surface is
exactly unchanged.

**Where and how much:**
- `ReferenceVoxelRenderer::subsurfaceDiffusionSamples` and
  `PolygonRenderOptions::subsurfaceDiffusionSamples`, both 16 by default; 0 = off.
- Both also switch off with the environment's subsurface switch.
- Polygons use their own mesh for the probes and, as elsewhere in the polygon reference, no sun
  shadows.
- The GPU version (a screen-space blur with the same profile) is not written yet.

**The reflection colour.** Reflection uses the base colour as the albedo; translucency uses the
subsurface colour. For a physically consistent material set them equal; separate colours remain an
artistic control.

## Measured (`tests/test_subsurface_diffusion.cpp`)

**The table against fresh walks** at seven points on and between its samples, and past its end: all
within 0.0035.

| A | t / d | μ | table | walk |
|---|---|---|---|---|
| 0.80 | 1.0 | 1.00 | 0.2699 | 0.2732 |
| 0.73 | 2.3 | 0.87 | 0.3204 | 0.3185 |
| 0.95 | 0.7 | 0.45 | 0.3478 | 0.3512 |
| 0.97 | 20 | 1.00 | 0.0579 | 0.0571 |

Also checked:
- the GPU table is the same numbers;
- transmission falls steadily past the end.

**Profile sampling:**
- the mean radius is 2.5 d and the radius CDF is exact;
- the share of the profile beyond a line matches brute-force sampling.

**A shadow edge on a marble block**, voxel renderer with 512 samples, against the exact Burley edge
1 − share(x):
- within 0.003 everywhere from −15 d to +14 d;
- with diffusion off the edge stays a step;
- with no shadow, diffusion changes nothing.

**The same edge from the random walk** (A = 0.8, sun overhead), relative to the lit side:

| x / d | walk | Burley |
|---|---|---|
| −4 | 0.064 | 0.052 |
| −2 | 0.140 | 0.130 |
| −1 | 0.244 | 0.223 |
| 0 | 0.511 | 0.500 |
| +1 | 0.773 | 0.777 |
| +4 | 0.936 | 0.948 |

So Burley is within 0.03 of the truth. Its tail is a little short, as part 2 found for the profile.

**A marble ball lit from the side (polygons):**
- light crosses the terminator: 0.013 → 0.049 just past it;
- the lit face keeps its light within a few percent. The cosine averaged over a curved face is a
  little lower, and that light is what crossed.

## Not yet

- **The GPU diffusion pass.**
- **A texture for the GPU table.** It is a static array for now: fine for compiling, but it should
  become a small 3-D texture (33 × 21 × 9) with hardware filtering once the GPU path has a host.
- **Light through a Subsurface object onto what is behind it.**
- **Per-channel scatter distances.**
