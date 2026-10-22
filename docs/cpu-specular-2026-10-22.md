# CPU specular shading (22 Oct 2026)

Before this change, the CPU reference renderers (the ones that actually draw) had no specular
light at all:
- voxels were pure Lambert: base x (sun + ambient);
- polygons were Lambert plus one hard-coded clear-coat highlight computed for a fixed view
  direction (0, 0, 1), so it never moved with the camera;
- polygon ambient was one flat colour, whatever way the surface faced;
- `roughness`, `metallic` and `specular` did nothing on the CPU.

The GPU shaders (`shaders/common/pbr_lighting.hlsli`) have a full Cook-Torrance model, but
nothing runs them yet.

## One model for both CPU renderers

`dve/render/surface_shading.hpp` mirrors the GPU functions line by line:
- `distribution_ggx`, `visibility_smith_ggx_correlated`, `fresnel_schlick`;
- `shade_direct_light` and `shade_clear_coat_direct`, returned as their two lobes (diffuse and
  specular).

Both the voxel and the polygon renderer now shade with it:

- **Direct sun.**
  - Diffuse is base x (1 − metallic) x (1 − F) x sun x cos, or the subsurface-diffused sun term
    where there is one.
  - Specular is the GGX lobe for the real view direction.
  - Metals reflect their base colour and have no diffuse.
  - Clear coat adds its own sharp lobe and dims the base through the coat (in and out).
- **Ambient (sky).**
  - Diffuse is the sky/ground hemisphere around the normal. It is occluded by AO, or comes from
    the traced GI, as before.
  - Specular reflects the sky along the mirror direction, scaled by the environment BRDF and
    Lagarde's specular occlusion from AO.
  - Polygons now get the hemisphere by normal, not a flat average.
- **Energy.** The ambient diffuse is multiplied by (1 − Es), where Es is the specular lobe's
  directional albedo. A white surface under a uniform sky therefore reflects exactly what it
  receives (tested to 1e-4).
- **Units.** The CPU draws albedo x irradiance (π x radiance), so the GPU lobes are multiplied
  by π (`kCpuRadianceScale`).

The environment BRDF is a 32 x 32 table computed once at start-up from the same D and V as the
direct lobe, so the two agree. It is within 0.014 of the numerical integral.

## Two shader changes (GPU and CPU alike)

- **"No specular" is now possible.** Fresnel's grazing term is multiplied by
  saturate(50 x F0.g), Unreal's convention. Nothing real reflects less than 2% head on, so
  specular below 0.25 (F0 below 0.02) fades the grazing reflection out too. `specular = 0` is
  now exactly Lambert. Materials at the default 0.5 are unchanged.
- **IBL energy conservation.** `environment_ibl.hlsli` now multiplies its diffuse by
  (1 − specular albedo), as the CPU does.

## What changes in existing scenes

Every material with the default `specular = 0.5` now shows:
- a 4% reflection head on, rising toward grazing angles;
- a sun highlight sized by its roughness.

Matte surfaces under the sun get slightly darker diffuse (the 4% that is reflected). The sky
reflection makes up for it at glancing angles.

Four existing tests compared exact Lambert values. Their materials now set `specular = 0`:
- the caustics review floor;
- the subsurface marble;
- two radiance-cascades fixtures.

## Known limits (same as the GPU)

- **Grazing overshoot.** Diffuse uses 1 − F at the half vector, so a white dielectric seen at a
  grazing angle can reflect up to about 1.2 x the sun it receives (NdotV 0.2, roughness 0.3).
  Facing views stay within 1%. The test bounds this.
- **Rough metals are dark.** Single-scattering GGX loses energy at high roughness: a rough white
  metal reflects about 30–80% instead of 100%. A multiple-scattering compensation would fix
  this. The table already has what it needs: specular x (1 + F0 (1 / Es − 1)).
- **Polygons have no sun shadows** in the polygon reference (unchanged), so their specular is
  never shadowed. In the voxel renderer, specular uses the point's own sun visibility.

## Tests

`tests/test_specular_shading.cpp` (`dve_specular_shading_tests`) checks:
- the closed forms at normal incidence (F0 D V and (1 − F0)/π);
- that metals have no diffuse;
- white-furnace energy for the sun and for a uniform sky;
- the table against the numerical albedo;
- in both renderers, a pixel at the mirror view and at the retro view equals π x (diffuse +
  specular) of the model for that view, to 0.2%, for standard and clear-coat materials. This
  is the check that catches the old fixed-view clear-coat highlight.
