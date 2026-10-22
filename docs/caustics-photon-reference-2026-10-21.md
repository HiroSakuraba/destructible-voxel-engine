# Caustics, part 2: light through glass and water, and a photon ground truth (21 Oct 2026)

Builds on part 1 (refractive materials). This is the reference that the real-time caustic map
(part 3) and the later GPU work are measured against.

## OpticalScene: following light through media

`include/dve/render/optical_scene.hpp` covers voxel instances (through the reference renderer's
exact voxel tracer) and polygon meshes (through `PolygonBvh`).

- **Refractive vs receiver.** Any material with `transmission > 0` is a medium. Every other surface
  is a receiver, where the light path ends.
- **At a medium's surface:**
  - exact dielectric Fresnel (unpolarised) and Snell's law;
  - total internal reflection is followed;
  - the transmitted part is scaled by `transmission` on the way in.
- **Inside:** Beer–Lambert absorption toward `attenuationColor`, with distances converted to metres.
  - **Voxel media** are crossed cell by cell until the material changes. Beyond can be air (exit),
    another medium (water into glass), or an opaque cell (a pool floor in the same object, which
    is a receiver). Other objects inside the medium are found too.
  - **Polygon media** use interpolated vertex normals, so a smooth mesh is a smooth lens. Leaving is
    hitting the same mesh from inside.
- **A stack of media**, so water → glass → water returns to water's index.
- **Two ways to follow light:**
  - `follow_light_transmitted`: deterministic, the transmitted branch weighted by 1 − F. The
    caustic map will use this.
  - `follow_light_sampled`: reflect or transmit chosen with the Fresnel probability, for full
    ground truth.

## Photons

`include/dve/render/caustic_photons.hpp`:

1. `caustic_light_region` places a rectangle facing the sun, above everything, covering the
   refractive objects as the sun sees them.
2. `trace_caustic_photons` shoots a stratified (jittered) grid of photons from it. Directions come
   from the sun disc when the sun has a size.
3. Each photon is followed through the media. Only light that went through or off a refractive
   object is stored, because direct light is the shadow term.
4. `CausticPhotonMap` turns photons back into irradiance (photons in a disc on the surface ÷ disc
   area), relative to full sun on a surface facing it.

## Checked against closed-form answers (`tests/test_caustic_photons.cpp`)

| case | measured | exact |
|---|---|---|
| glass slab, sun overhead: (1 − 0.04)² | 0.9242 | 0.9216 |
| same slab, colour halving every 0.25 m, 0.5 m thick: × 0.25 | 0.2311 | 0.2304 |
| pool (water voxels on a floor, one object), 40° sun: cos 40° × (1 − F(40°)) | 0.7500 | 0.7475 |
| glass ball, R = 2: power through it = ∫ (1 − F)² over the cross-section | 10.486 | 10.500 |

Also checked:
- **Ball focus:** with the focus on the floor (`nR / 2(n − 1)` from the centre), the floor is
  about 100 × full sun; 1.5 units out it gets 0.1.
- **Finite sun:** a 0.05 rad sun lowers that peak to 54 × and keeps the energy.
- **Reflections:** including them leaves the focus unchanged within 15 %; the reflected light lands
  elsewhere.
- **Basics:** Fresnel (4 % at normal incidence, total internal reflection past 41.8°), Snell and
  Beer–Lambert on their own.

The whole test runs in about 4 s.
