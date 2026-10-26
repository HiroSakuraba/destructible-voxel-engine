# Voxel texture sets (25 Oct 2026)

Stacked on `feature/atmosphere-gpu`. This is the first of the texturing branches; it is the
foundation for triplanar displacement, which needs height maps.

Before this change, a voxel material was one flat colour, roughness and metallic value
everywhere. The only maps it could carry were a normal map and the procedural surface recipes.

## The material

`VoxelMaterialDefinition` gains four optional maps and a shared projection:

| field | contents | applied as |
| --- | --- | --- |
| `baseColorMap` | sRGB colour, alpha | × `baseColor` (rgb and alpha) |
| `ormMap` | R occlusion, G roughness, B metallic (packed as in glTF) | occlusion scales the ambient light and its specular reflection; roughness and metallic multiply the material's |
| `heightMap` | R: 0 deepest … 1 highest | sampled for parallax and displacement (next branch) |
| `emissiveMap` | sRGB colour | × `emissive` |
| `textureTileMeters` | world size of one repeat (default 1 m) | |
| `textureSharpness` | triplanar blend exponent, 1 … 64 (default 4) | |

The normal map keeps its own tile size, strength and sharpness, as before.

**Projection.** Maps are projected triplanar in the object's own space, with the same
projections as the voxel normal map: X → (z, y), Y → (x, z), Z → (x, y). So patterns move with
the object.

**Image type.** Images use the existing `VoxelNormalMap` type, now with a `role` and a mip chain.
`make_voxel_image(name, w, h, texels, role)` builds every level down to 1 × 1:

| role | used for | mip reduction |
| --- | --- | --- |
| Color | base colour, emission | averages in linear light (sRGB decoded, then re-encoded) |
| Data | ORM, height | averages raw bytes |
| Normal | normal maps | averages and renormalises vectors |

The role is part of the content hash. Normal maps made by `make_voxel_normal_map` keep their
old hash.

**Filtering.** The level comes from the pixel's footprint on the surface, log₂(footprint /
texel size), and sampling is trilinear (bilinear within a level, blended between levels). A fine
checker seen from far away filters to its mean instead of shimmering.

**Importing.** `import_voxel_texture(path, role)` and `decode_voxel_texture(bytes, name, role)`
read PNG or JPEG through the asset pipeline, with no colour conversion on import. The role
decides how the image is read.

## Where the maps apply

- Voxel reference renderer: both the primary and the radiance-cascades paths.
- Polygons that carry voxel materials (meshes made from voxels): colour, roughness, metallic and
  emission maps; occlusion scales the polygon AO.
- The path tracer's secondary hits on voxels and on such polygons. It skips the occlusion map,
  since the path tracer finds occlusion itself.

## Saving

Each format writes texture sets only when a material has one, so other files keep their bytes
and hashes:

| where | how |
| --- | --- |
| DVOX | version 1.10: per material, a mask of the maps present, the projection, then each map |
| Game saves | section `dve.material_textures`: every distinct image once, with its role, then per material four slot indices and the projection |
| Editor material library | version 10 |
| Hashes | runtime scene, brick palette and polygon material hashes include each map's content hash and the projection |

## Checks

`dve_voxel_textures_tests`:
- **Mips.** A 16 × 16 image gets four levels down to 1 × 1. A one-texel checker reduces to
  sRGB 188 (linear 0.5) as colour and to 128 as data, and the role changes the hash.
- **Sampling.** Constant images sample constant at any level, texel centres are exact, and
  fractional levels blend linearly.
- **Base colour map.** Pixel = 0.8 × linear(128/255) × sun × cos, exact.
- **Occlusion map.** At 128/255, the ambient-only pixel scales by exactly 128/255.
- **Emission map.** The emission pass = emissive × linear(map).
- **Roughness map.** It changes the highlight.
- **Filtering.** A 0.8 mm checker seen from 40 m is within 0.6% of its mean everywhere; close
  up it stays sharp.
- **Meshes made from voxels.** A polygon quad carrying the maps renders the same colour.
- **DVOX.** v1.10 only with maps. The maps, roles, projection and hashes round-trip, and the
  mips are rebuilt on read.

`test_game_save` round-trips two maps (same texels, different roles), and re-encoding gives the
same bytes. `test_material_optics` round-trips the editor library at version 10.

## Not in this change

- **GPU material buffers** don't carry the maps yet, which is the same situation as voxel normal
  maps.
- **No editor UI or Lua** for assigning maps yet.
- **Height maps are stored and sampled here, but not drawn yet.** Displacement and parallax are
  the next branch.
