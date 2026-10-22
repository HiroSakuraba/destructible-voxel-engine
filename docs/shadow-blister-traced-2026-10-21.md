# The shadow blister in traced soft shadows (21 Oct 2026)

Part 1 of 2. Part 2 brings the same effect to the cascaded shadow maps (bitmask soft shadows).

## The effect

The sun is a disc, not a point. A spot on the ground is lit by the part of the disc it can see.
When two shadow edges come close, one object can hide the left part of the disc while the other
hides the right part, so the gap between them gets darker than either shadow alone. The two
penumbrae swell toward each other and join before the dark cores touch: the shadow blister.

The light lost is the **union** of what each object hides. It is not the darker of the two
shadows, and not their product.

## Where DVE stood

Traced soft shadows (`generate_shadow_rays.hlsl`, and the CPU reference) already send rays to
points on the sun disc and count how many get through, which is the union. So the physics was
right. Two things could hide the effect:

- **Sampling.** Disc points came from a polar map (`sqrt(u)`, `2πv`), which bunches strata into
  thin rings. On the CPU reference, only the angle was rotated per pixel, so every pixel used the
  same set of radii, and the test below showed a 0.04 bias at the blister.
- **Denoising.** The spatial filter blurred shadows with a fixed 5x5 Gaussian (sigma 1.5 px),
  whatever the penumbra. A blister two or three pixels wide (contact shadows, objects close to
  the ground) was flattened.

## What changed

- **Concentric disc sampling** (`ConcentricSquareToDisk`, Shirley–Chiu) for sun-disc rays on the
  GPU and the CPU reference: equal-area, keeps strata compact. The CPU reference now rotates both
  coordinates per pixel exactly like the GPU (`RotatedHammersley2D` with `Hash2(seed)`).
- **Penumbra width from the resolve.** `resolve_shadows.hlsl` writes `gShadowPenumbraMeters`
  (u3): `2 tan(sun angular radius) × mean distance to the occluders the soft rays hit`, or 0 when
  none were hit.
- **Penumbra-aware shadow blur.** `spatial_lighting_denoise.hlsl` reads it (t5). The shadow
  channel's sigma is a quarter of the widest penumbra under the filter, in pixels at the centre's
  depth, clamped to [0.25, the old sigma]. Narrow penumbrae stay sharp; wide ones are blurred
  exactly as before. GI and AO are unchanged. The host sets `gDenoiseRadiansPerPixel` (the old
  padding slot; `denoise_radians_per_pixel(fov, height)` computes it). Left at 0 it keeps the old
  fixed blur.
- `reference_sun_visibility(...)`: the reference renderer's per-pixel sun visibility, callable at
  one point (tests, tools).

The temporal accumulation is unchanged.

## The measurement

`tests/test_shadow_blister.cpp`. Two slabs, 20 voxels above the ground with a 2-voxel gap, sun
overhead, angular radius 0.1. Exact visibility is a numeric integral over the sun disc
(360 × 360 cells, ray against each slab).

| at the gap centre | visibility |
|---|---|
| exact (union) | 0.578 |
| CPU reference (1024 rays) | 0.579 |
| darker of the two shadows | 0.789 |
| product of the two | 0.623 |

Across the whole profile the reference stays within 0.03 of the exact value.

Denoiser, on a temporally accumulated estimate (128 rays per pixel):

| | old 5x5 blur | penumbra-aware |
|---|---|---|
| narrow blister (2 px penumbra): error at the gap centre | 0.262 | 0.065 |
| narrow blister: RMS error | 0.052 | 0.015 |
| wide penumbra (300 px): RMS error | 0.0014 | 0.0014 (identical) |

## Code

- `shaders/common/random_sampling.hlsli`: `ConcentricSquareToDisk`.
- `shaders/generate_shadow_rays.hlsl`, `resolve_shadows.hlsl`, `spatial_lighting_denoise.hlsl`;
  manifest bindings `gShadowPenumbraMeters` (u3) and `gDenoiseShadowPenumbraMeters` (t5).
- `ray_lighting_reference.hpp`: `concentric_square_to_disk`, `shadow_penumbra_width_meters`,
  `denoise_shadow_visibility` (CPU mirror of the shadow channel), `denoise_radians_per_pixel`.
- `voxel_reference_renderer.hpp`: `reference_sun_visibility`.
