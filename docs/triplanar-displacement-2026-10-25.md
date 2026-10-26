# Triplanar displacement (25 Oct 2026)

Stacked on `feature/voxel-texture-sets`. That branch added height maps to voxel materials.
This one draws them, in three ways:

1. traced displacement on voxels, with a parallax fallback;
2. triplanar parallax on polygons;
3. triplanar heights for mesh displacement.

## 1. Voxels: traced displacement, parallax fallback

`VoxelMaterialDefinition` gains two fields:

| field | meaning |
| --- | --- |
| `displacementDepthMeters` | how deep the height map carves, 0 … 10 m (0 = off). The depth used is capped at half a voxel. |
| `displacementMode` | 0 Traced (default), 1 Parallax |

A material displaces when it has a `heightMap` and a depth above 0. The height map uses the
texture set's projection, tile size and sharpness.

**Traced.** The height carves inward under each exposed face. A point in the voxel is solid when
its depth below every exposed face is at least (1 − h) × depth. So h = 1 is the original cube
face and h = 0 is the deepest carve. Unexposed sides (touching another solid voxel) stay flat.

The scene tracer (`VoxelSceneTracer`) marches rays through the carved cell and refines the hit.
The normal is n − depth × ∇h. A ray that only crosses carved-away space continues to whatever is
behind it.

Every tracer user sees the relief: the image, outlines, shadows, AO, GI, light shafts,
volumetric fog shadows and the depth pass. See `dve/voxel_displacement.hpp`
(`trace_displaced_voxel`).

**Parallax.** This is shading only (`voxel_parallax`). A parallax-occlusion march on the hit face
shifts where every map is read and tilts the normal. The outline, depth and shadows stay cubic.

**Fallback.** `RenderEnvironment::displacementTraceDistanceMeters` (default 100 m) is the radius
around the camera inside which Traced materials are traced. Beyond it they use parallax, which
gives exactly the same result as a material set to Parallax.

Sun visibility rays and the path tracer use an unlimited radius, so a shadow doesn't change with
the camera.

## 2. Polygons: triplanar parallax

Before this change, parallax on polygon materials was UV-only. `validate_material_mapping_settings`
refused height modes on `WorldTriplanar` and `ObjectTriplanar` mappings. That check is gone:

- **Mapping space.** The march runs in the tangent plane of the mapping space (world or object).
  It uses the projection's units, where one unit is 1 / `triplanarScale`, so `heightScale` means
  the same as it does in UV.
- **Height sampling.** Each step reads the height triplanar.
- **Shared march.** The 2D march is the same as for UV (`apply_parallax_mapping`), so offset,
  steep and occlusion parallax all work.
- **Result.** The result is a position shift. Every texture lookup for the fragment uses the
  shifted world and object positions: base colour, ORM, emission, opacity, detail maps, normal
  maps, layer masks and procedural surface detail. Lighting keeps the true surface position.
- **Relief.** Relief depth (`surfaceDetail.reliefDepthMeters`) now works on triplanar mappings
  too.
- **`heightScale` units.** For triplanar, `heightScale` is in projection units before
  `baseTransform`.

**Meshes made from voxels.** A polygon that carries a displaced voxel material gets
`voxel_parallax` on its axis-aligned faces, whichever mode is set, because a mesh isn't traced as
voxels.

**Fix along the way.** `ObjectTriplanar` lookups for base colour, ORM, emission, opacity and
detail now pass the object-space normal. Before, they passed the world normal, so the projection
weights were wrong on rotated objects.

**Cost estimate.** The material authoring cost estimate counts three height reads per step for
triplanar parallax.

**GPU.** Live parallax is still CPU reference only, as before.

## 3. Mesh displacement

- **Triplanar heights.** `build_displaced_polygon_asset` reads heights triplanar when the
  binding's mapping is triplanar. It samples at the vertex position and normal, with
  `triplanarScale`, `triplanarBlendSharpness` and `baseTransform`, and blends as the renderer
  does. A cooked asset has no world placement, so `WorldTriplanar` uses object space here.
- **Voxel height maps.** A material with no height binding but with a voxel `heightMap` uses
  that map as the height source (`sample_voxel_height`).
- **Editor.** The editor's mesh displacement accepts such materials.
- **Stats.** `PolygonDisplacementStats::triplanarHeightVertices` counts the vertices whose height
  was read triplanar.

## Saving

| where | how |
| --- | --- |
| DVOX | version 1.11: per material, depth (f32) and mode (u8) after the texture set, only when some material displaces |
| Game saves | section `dve.material_displacement` (object id, material index, depth, mode) |
| Editor material library | version 11 |
| Hashes | runtime scene, brick palette and polygon material hashes include depth and mode |

## Checks

`dve_triplanar_displacement_tests`:

**The carved cell**
- **Constant height.** A constant height carves to exactly (1 − h) × depth.
- **Ramp.** A ramp tilts the normal by exactly depth × dh/dx.
- **Fully carved edge.** It lets rays through.
- **Unexposed side.** It stays flat.
- **Depth cap.** The depth is capped at half a voxel.

**The tracer**
- **Carved space.** Rays pass through it to the voxel behind.
- **Focus radius.** It switches tracing off beyond the radius.

**The renderer, traced**
- **Depth pass.** It moves by exactly the carve (0.02996 m).
- **Outline.** A carved cube's outline shrinks to (2/3)² = 0.444 of the flat one.
- **Grooves.** They shadow themselves at a 19° sun: 1752 pixels. Parallax gives none.

**Parallax**
- **Read point.** It shifts by exactly lateral / cos × depth (−0.01494).
- **Depth.** It is unchanged.
- **Fallback.** Beyond the trace distance, traced materials render the same as parallax ones.

**Polygons**
- **World triplanar.** Parallax on a quad whose UVs equal the projection gives the same image as
  UV parallax (difference 0.00000). It moves the stripes by up to 0.32 against no parallax.
- **Object triplanar on a rotated object.** It matches UV parallax (0.00000).
- **Displaced voxel material on a mesh.** It marches and shifts the maps.

**Mesh displacement**
- **Triplanar heights.** They follow the vertex position when the UVs are constant; UV heights
  stay flat.
- **Voxel height map.** A constant voxel height map lifts by exactly (h − reference) × scale, and
  no height binding is reported missing.

**Formats**
- DVOX is written at 1.11 only with displacement.
- `test_game_save` round-trips depth and mode in `dve.material_displacement`.
- `test_material_optics` round-trips the editor library at version 11.
- `test_material_mapping` now accepts parallax on triplanar mappings.

## Not in this change

- **GPU.** The GPU voxel renderer and GPU material buffers don't carry the maps or displacement
  yet.
- **Polygon mesh displacement normals.** For texture heights, displacement still only moves
  vertices; only procedural noise updates the normal, as before.
- **Editor UI and Lua.** There is no UI or Lua for the depth and mode yet.
