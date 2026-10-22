# Render passes and distance fog (22 Oct 2026)

Stacked on `feature/cpu-specular`. The CPU reference renderers can now split a frame into the
passes a compositor works with, and add distance fog from the frame's depth.

## The passes

Passes are off by default. Turn them on with `PolygonRenderTarget::enable_passes()`; both
renderers, the hybrid reference and `composite_hybrid_layers` then fill
`target.passes` (`RenderPassBuffers`).

**Light passes.** At every pixel these sum to the image (tested to 1e-5):

| pass | what it holds |
| --- | --- |
| `directDiffuse` | sun diffuse, plus caustic light |
| `directSpecular` | sun highlights, including the clear coat |
| `indirectDiffuse` | sky / GI diffuse (AO-occluded) |
| `indirectSpecular` | sky reflections |
| `emission` | the glow pass: emissive colour, Unlit and Emissive materials (and material debug views) |
| `transmission` | foliage and subsurface light coming through |
| `background` | the sky / clear colour where nothing was drawn |
| `fog` | what the distance fog added |

**Translucent surfaces** blend every light pass by their alpha, exactly as they blend the image.
So the sum still holds through glass.

**Data passes.** These are not part of the sum and come from the nearest opaque surface:

- `shadow`: the direct sun light the shadows removed. Image + shadow = the same frame rendered
  with shadows off (tested to 1e-4). Tint or scale it in post to colour or soften shadows.
  - On subsurface-diffused materials the point's own share is unshadowed but its neighbours'
    are not, so the pass is approximate there.
  - It is zero on polygons, because the polygon reference casts no sun shadows.
- `sunVisibility`: 1 lit, 0 fully shadowed, with shadow strength applied.
- `normal`: the world-space shading normal.
- `depthMeters`: the Z-depth pass, camera-space Z in metres.

**Empty pixels** have depth +∞, normal 0 and visibility 1.

**Saving passes to files.** `write_render_passes_pfm(target, directory, prefix, farPlaneMeters)`
writes `beauty.pfm` and one file per pass:
- 32-bit float PFM, which Nuke, Photoshop, Krita, GIMP and OpenImageIO all read;
- colour passes are RGB; depth and sun visibility are greyscale;
- empty depth pixels hold `farPlaneMeters`.

`polygon_render_demo` takes an optional 4th argument, a directory, and saves its passes there.

## Distance fog

New in `RenderEnvironment`:
- `fogDensityPerMeter` (default 0 = off, at most 10);
- `fogStartMeters` (default 0);
- `fogColor` (radiance, like `skyColor`).

The same settings are in Lua:
- `FogDensityPerMeter` and `FogStartMeters` (scalars);
- `FogColor` (vector);
- `fog_*` in the environment table.

**Saved script state.** It is now version 3 and stores the fog after the environment block.
Versions 1 and 2 still load, with fog off.

**How it works.** `apply_distance_fog(target, camera, environment, metersPerUnit)` runs after
the frame is drawn:
- For each drawn pixel, d is the distance in metres along that pixel's ray (from the depth),
  and T = exp(−density × max(0, d − start)).
- The pixel becomes image × T + fogColor × (1 − T).
- The light passes are scaled by T and the fog pass gets the rest, so the passes still sum.
- The sky is left alone.

The hybrid reference and the CPU player apply it. The standalone voxel and polygon renderers
don't, because a polygon layer drawn over a voxel layer would otherwise fog the voxels twice.

Tested exactly: every fogged pixel matches the formula with the exact ray distance.

## Not in this change

- Fog is uniform: there is no height falloff or in-scattered sunlight. Both would be small
  additions to `apply_distance_fog`.
- Depth of field and atmospheric blur aren't implemented. The depth pass is what a compositor
  needs for them.

## Tests

`tests/test_render_passes.cpp` (`dve_render_passes_tests`) renders a hybrid scene with sun,
shadows, AO'd sky, a metal wall, an emissive block, a clear-coat polygon box, a translucent pane
and sky. It checks:
- the sum;
- image + shadow = unshadowed;
- the pane's blended passes;
- depth (voxels and polygons) and normals;
- empty pixels;
- fog against the exact formula;
- the PFM files, read back.

`test_game_save` round-trips the fog through script state v3.
