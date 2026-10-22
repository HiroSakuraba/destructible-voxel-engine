# Volumetric fog and light shafts (22 Oct 2026)

Stacked on `feature/atmosphere`. This adds a height fog that scatters sunlight. It is
ray-marched with a shadow ray toward the sun at every step, so air in shadow stays dark, lit air
glows, and light through gaps forms shafts (god rays).

## Settings (`RenderEnvironment`; Lua scalars, script state version 5)

| setting | default | |
| --- | --- | --- |
| `volumetricFogDensityPerMeter` | 0 (off) | extinction at and below the base height |
| `volumetricFogHeightFalloffPerMeter` | 0.05 | above the base, density × exp(−falloff × height above it); 0 = uniform |
| `volumetricFogBaseHeightMeters` | 0 | world y in metres |
| `volumetricFogAlbedo` | 0.9 grey | share of extinction that scatters (colour) |
| `volumetricFogAnisotropy` | 0.6 | Henyey-Greenstein g: forward scattering, so the fog glows toward the sun |
| `volumetricFogMaxDistanceMeters` | 100 | how far from the camera the march reaches |
| `volumetricFogSamples` | 32 | steps per pixel (and shadow rays) |

## How each pixel is computed (`render/volumetric_fog.hpp`)

The ray runs from the camera to the surface (sky pixels: to the max distance) in equal steps.
For each step:
- **Optical depth.** τ is exact, from a closed form for "constant below the base, exponential
  above".
- **What the step adds.** T × (1 − e^−τ) × albedo × light, where T is the transmittance up to
  that step. This is the exact integral when the light is constant across the step, for any
  density profile.
- **The light.**
  - Sun: π × HG(g, view · sun) × the sun at the ground. This goes through the physical atmosphere
    in Physical sky mode, so sunset shafts are orange.
  - Visibility: from one shadow ray at a jittered point in the step, through voxels and polygons
    (`HybridOcclusionScene`), with shadow strength applied.
  - Sky light: the mean of the sky irradiance from above and below, × the GI intensity (none with
    GI off). It only counts where a second ray, straight up from the same point, reaches open sky.
    So fog inside a room is lit only by what comes through its openings, not by the whole sky.

The pixel keeps T of its light and gains the scattered light:
- the light passes are scaled by T;
- the scattered light goes into the fog pass, so the passes still sum.

**Determinism.** The jitter is hashed from the pixel and a frame seed, so the image doesn't
depend on the thread count. The CPU player advances the seed every frame, so the shaft edges
average out over time.

**Order in a frame.** Sky, then voxels and polygons, then aerial perspective, then volumetric
fog, then the old distance fog. This runs in `render_hybrid_reference` and the CPU player.
`HybridReferenceRenderStats::volumetricFog` reports pixels, samples and shadow rays.

## Checks (`dve_volumetric_fog_tests`)

| check | result |
| --- | --- |
| optical depth closed form vs numerical (above, below and across the base, climbing, descending, level) | 9e-8 |
| uniform fog: in-scattering and transmittance vs closed form | 1e-6 |
| sky light: albedo × mean irradiance × (1 − T) | exact |
| height fog transmittance | exact |
| half-shadowed ray, boundary on a step edge | exact; one shadow ray per step |
| boundary inside a step, averaged over 3000 jitters | exact to 1e-6 |
| hybrid frame: shaft under a slot in a roof vs shadowed air | 0.283 vs 0.048 |
| same image for 1 and 4 threads | yes |
| passes still sum | 1e-7 |
| density 0: nothing runs | yes |

## Limits

- **Shadow sampling.** It is one ray per step, and the steps are equal. A long march in thin fog
  with few samples gives banded shafts. Raise the samples, shorten the max distance, or rely on
  frame-to-frame jitter.
- **Fog beyond the max distance is ignored.** Use the distance fog or aerial perspective for the
  far field.
- **No light other than the sun and the sky:** no point lights, and no light bounced off surfaces
  into the fog.
- **CPU only.** The GPU branch covers the atmosphere tables, not this march.
