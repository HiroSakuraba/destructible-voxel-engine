# Subsurface, part 1: translucency (22 Oct 2026)

Thin wax, leaves, ears and jade glow when the sun is behind them. Before this change:
- the material side already existed: the `Subsurface` shading model, `subsurfaceScatterDistanceMeters`,
  `subsurfaceColor` and the global `subsurfaceMaxDistanceMeters`;
- but the CPU reference renderers (the ones that draw) ignored it;
- the GPU shader had the term, but no host dispatches it, and it had two bugs (below).

## The model (`include/dve/render/subsurface.hpp`)

At a point P on a Subsurface material, facing away from the sun:

1. **March toward the sun** through the same material to where it ends (point E, outward normal
   N_E). Only open air beyond lets the sun in; another solid material blocks it.
2. **Thickness d** is the distance from P to the plane of the sunlit face at E. For a slab this is
   the slab's thickness at any sun angle, because the slant is already in the cosine at E.
3. **Light on the shaded side** = subsurfaceColor × sun × cos(N_E, sun) × V(E) × T(d / s), where:
   - V(E) is the sun's visibility at E, traced from there (the far side can be shadowed too);
   - s is the scatter distance;
   - T is Burley's normalized-diffusion transmittance: T(d) = ¼ e^(−d/s) + ¾ e^(−d/3s). This is
     Burley's diffusion profile summed over a plane at depth d. It is the same thickness form
     Unity HDRP and Unreal use for subsurface-profile transmission.

Points facing the sun get nothing extra: their light is the ordinary diffuse term. Softening that
front-lit light (light spreading sideways under the surface) is part 2.

Sources:
- Burley, *Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering* (SIGGRAPH
  2015 course notes);
- Christensen and Burley, *Approximate Reflectance Profiles for Efficient Subsurface Scattering*
  (Pixar, 2015).

## Where it runs

- **`ReferenceVoxelRenderer`**, both shading paths (per pixel, and radiance cascades):
  - the march crosses the instance's cells;
  - V(E) is the same sun-visibility test as every other voxel: hard, soft or hybrid; voxel and
    polygon occluders; shadow strength.
  - New stats: `subsurfaceMarches`, `subsurfaceExits`.
- **`ReferencePolygonRenderer`**:
  - the march is a ray against the mesh's own BVH, built only for assets with a Subsurface
    material;
  - normals are interpolated, so a smooth mesh gives smooth thickness;
  - the polygon reference has no sun shadows, so the far side counts as unshadowed, the same as
    the near side's direct term;
  - `PolygonRenderOptions::metersPerUnit` converts thickness to metres; the hybrid reference and
    the CPU player pass their metres per voxel.
- **The GPU shader** (`shaders/common/subsurface.hlsli`, `shade_primary.hlsl`) now matches. It had
  two bugs:
  - **It marched along the surface normal, not toward the sun.** It found the opposite face, not
    the one the sun actually reaches, so it had no proper cosine there.
  - **It multiplied by the near side's shadow.** For a backlit point that is the object's own
    shadow, so the glow was almost always zero.

  It now marches toward the sun and fires one hard sun ray from E.

Not yet:
- the Burley sideways spread for front-lit light (part 2);
- light through a Subsurface object onto what is behind it (it still casts a full shadow);
- per-channel scatter distances.

## Measured (`tests/test_subsurface_translucency.cpp`)

- **Burley closed form against the profile summed numerically over the plane** at d = 0, 0.1,
  0.3, 0.8 (s = 0.2): equal to five digits.
- **March:** on a 3-voxel slab the chord is 3 overhead and 3 / cos 50° at 50°, and the thickness is
  3 at both. Past the maximum distance, or under a stone cap, no light gets through.
- **Underside of a 0.3 m wax slab:** the voxel renderer, the radiance-cascades path and the polygon
  renderer (the same slab as a box mesh) all give the expected value within 0.03 %, at 0° and 50°.
  The offset comes from starting 0.001 inside the surface.
- **Zero when it should be:**
  - a roof shadows the far side (with shadow strength 0.85 the light is 15 %);
  - a stone cap;
  - too thick;
  - subsurface switched off;
  - not a Subsurface material.
- **Smooth ball:**
  - from the bottom pole the light crosses the full diameter and enters at the top pole;
  - just past the terminator, the thickness is under a quarter of that.
- **GPU source:** it marches toward the sun, works on the shaded side only, uses Burley's form, and
  traces the far side's visibility.
