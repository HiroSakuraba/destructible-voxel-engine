# Voxel normal maps (22 Oct 2026)

Stacked on `feature/render-passes`. Before this change, polygons could already have normal maps
(UV or triplanar). Voxel materials had nothing, so every voxel face was perfectly flat.

## The material

`VoxelMaterialDefinition` has four new fields:

| field | meaning |
| --- | --- |
| `normalMap` | `std::shared_ptr<const VoxelNormalMap>`; null = none |
| `normalMapTileMeters` | world size of one repeat (default 1 m) |
| `normalMapStrength` | 0 flat, 1 as authored, up to 4 |
| `normalMapSharpness` | triplanar blend exponent, 1 soft to 64 hard (default 4) |

**`VoxelNormalMap`** holds an RGBA8 tangent-space image:
- xyz = 2 × rgb − 1, with +y up (the OpenGL / glTF convention);
- immutable, and shared between material copies;
- carries a content hash, so material hashes don't rehash the texels.

**Making one.** Use `make_voxel_normal_map(name, w, h, texels)`. It accepts 1–4096 pixels per side.

**Importing one.** `import_voxel_normal_map(path)` (or `decode_voxel_normal_map(bytes, name)`
for in-memory data) reads a PNG or JPEG through the asset pipeline's decoder:
- there is no sRGB conversion, because normal maps are data;
- the map is named after the file.

## How it's drawn

Voxels have no UVs, so the map is projected triplanar in the object's own space (metres /
tile):
- the three projections are swizzled onto their axes with the same swizzle as the polygon
  renderer's triplanar normal maps;
- they are blended by |normal|^sharpness;
- an axis-aligned face uses only its own projection;
- because the projection is in object space, the pattern moves with debris and moving objects.

**What follows the bent normal:**
- the sun's diffuse cosine;
- the specular highlight;
- the sky reflection;
- the normal pass.

**What keeps the geometric normal:**
- shadow, AO and GI rays;
- the ambient irradiance;
- the subsurface-diffused sun term (an average over the surface around the point).

A bent normal is never allowed to point behind its face.

## Saving

Each format writes normal maps only when a material has one, so files without them keep their
exact bytes and hashes.

| where | how |
| --- | --- |
| DVOX | version 1.5: per material, a flag, then name, size, parameters and texels. Readers accept 1.0–1.5. |
| Game saves | a `dve.material_normal_maps` section: each distinct image once, then (object, material, image, parameters) references. Materials also compare different from their source asset when only the map differs. |
| Editor material library | version 5: a map record per material, texels in hex |
| Hashes | runtime scene, brick palette and polygon material hashes include the map's content hash and parameters when present |

## Not in this change

- **GPU material buffer.** The GPU material buffer and the HLSL don't sample voxel normal maps.
  Nothing runs those shaders yet; it's a follow-up when they get a host.
- **Lua.** There is no Lua setter for the map. Maps come from assets, the editor library and
  saves.
- **Green-channel convention.** DirectX-style maps (+y down) need their green channel inverted
  on import. There is no switch for that yet.

## Tests

`tests/test_voxel_normal_maps.cpp` (`dve_voxel_normal_maps_tests`) checks:
- strength 0 gives exactly the image without a map, and a flat map stays within 0.3%;
- a constant tilted map lights the floor exactly as the tilted normal would (to 1e-4), and the
  normal pass shows the bent normal;
- the pattern repeats every tile;
- bent normals stay unit length and in front of side and bottom faces;
- a bumpy map varies the shading;
- moving the object and camera together gives the same image (to 5e-6);
- DVOX v1.5 round-trips the map, while files without maps stay at v1.2;
- invalid parameters are refused;
- a hand-built PNG imports with unchanged texels;
- missing or undecodable files and oversized images are refused.

Two existing tests gained checks:
- `test_game_save`: one image is stored once for two materials, round-trips, and re-encodes to
  the same bytes;
- `test_material_optics` (editor library): v4 stays v4 without maps, and v5 round-trips.
