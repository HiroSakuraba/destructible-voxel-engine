# Bitmask soft shadows for the cascaded shadow maps (21 Oct 2026)

Part 2 of 2. Builds on part 1 (traced shadows: sampling, denoiser, blister reference test).

## Why the shadow maps needed a new filter

The shadow blister happens when two penumbrae meet. One object hides part of the sun disc and the
other hides another part, so the gap between them gets darker than either shadow alone. The light
lost is the **union** of what each object hides.

The cascaded shadow maps used PCF:
- a fixed 3x3 to 9x9 comparison kernel, so softness ignored the sun's size and how far away the
  blocker is;
- the darker of the static and dynamic layers, so a static wall and a moving character never
  combine.

No better sampling fixes that. PCSS (one average blocker depth per pixel) gets the softness roughly
right, but it fails exactly where two blockers overlap.

## Bitmask soft shadows

Based on Schwarz & Stamminger, "Bitmask Soft Shadows" (2007), simplified for a directional sun.

1. The sun disc is 64 samples, kept as a 64-bit mask.
2. For a receiver, every shadow-map texel in front of it is projected back onto the sun disc. That
   gives a small square, scaled by `1 / (distance to the texel × tan(sun radius))`.
3. The samples inside each square are OR-ed into one occlusion mask, over all texels and both
   layers (static and dynamic).
4. Visibility is the uncovered fraction of the disc.

OR-ing is the union. So the blister appears, penumbra width follows the blocker distance and the
sun's size, and static and dynamic occluders combine properly.

**Fast square-to-mask.** The 64 samples form a rook pattern: sample *i* has x slot *i* and y slot
`(19 i + 9) mod 64`, and the 52 inside the unit disc count. With bits in x order, "x inside the
square" is a run of bits. "y inside the square" is the difference of two entries of a 65-entry
prefix table. So each texel costs a few integer operations, not 64 tests.

**Search.** A first pass reads every other texel over a square of `softShadowSearchTexels` (default
6) to find the nearest blocker. That sizes the real pass: texels within `distance × tan(sun radius)`,
capped at the same limit. Penumbrae wider than about twice the cap are clipped. The cost is at most
13 × 13 texels × 2 layers, plus the search, per pixel: much more than PCF's 3 × 3.

## Settings and constants

- `CascadedShadowSettings::softShadowFilter`: `Bitmask` (default) or `Pcf`.
- `CascadedShadowSettings::softShadowSearchTexels`: 1..16.
- The resolve's cbuffer now carries:
  - the filter and the search size, in the old padding slots;
  - `tan` of the sun angular radius (`RenderEnvironment::shadowSoftnessRadians`);
  - each cascade's texel size and depth range in metres.

  It is mirrored by `GpuCascadedShadowConstants` (96 bytes), and
  `make_gpu_cascaded_shadow_constants(plan, elementCount, sunAngularRadius)` fills it.
- Nothing in this repository dispatches `cascaded_shadow_resolve.hlsl` yet (the live material shader
  still binds the atlases as placeholders), so the new filter takes effect when a host wires the
  resolve.

## The measurement

`tests/test_bitmask_soft_shadow.cpp`. A CPU shadow map (0.25 m texels) of two slabs 21 m above the
ground with a 2 m gap: the left slab in the static layer, the right in the dynamic layer. Sun
straight overhead, tan(radius) = 0.1. The exact answer is a numeric integral over the sun disc.

| at the gap centre | visibility |
|---|---|
| exact | 0.584 |
| bitmask (both layers) | 0.577 |
| bitmask, left slab alone / right slab alone | 0.788 / 0.788 |
| PCF r = 4 | 0.889 |

Over the whole profile, bitmask stays within 0.024 of the exact value; PCF is off by up to 0.31.
The penumbra behind a straight edge widens from 0.28 m to 0.56 m when the blocker goes from 4 m to
8 m up (geometry predicts 0.29 and 0.58).

## Code

- `shaders/common/cascaded_shadow_map.hlsli`: `SampleCascadedShadowBitmask`, `SunDiscBox` and the
  tables.
- `shaders/cascaded_shadow_resolve.hlsl`: `CascadeVisibility` picks the filter; cascade blending
  unchanged.
- `include/dve/render/bitmask_soft_shadow.hpp` / `src/render/bitmask_soft_shadow.cpp`: the exact
  CPU mirror, plus a PCF mirror for comparison.
- `cascaded_shadow_map.hpp`: `CascadedSoftShadowFilter`, the settings, `GpuCascadedShadowConstants`,
  `make_gpu_cascaded_shadow_constants`.
- The test checks that the HLSL tables match the C++ ones bit for bit, and that the cbuffer field
  order matches the struct.
