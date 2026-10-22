# Refractive materials: transmission, IOR, attenuation (21 Oct 2026)

Caustics, part 1. Light can only bend into caustics if a material lets it in. DVE materials had
no index of refraction, and "translucent" only meant alpha blending. This adds the optical
properties, following glTF's KHR_materials_transmission, KHR_materials_ior and
KHR_materials_volume.

## Fields (`VoxelMaterialDefinition`)

| field | range | meaning |
|---|---|---|
| `transmission` | 0..1 | fraction of the light that is not reflected and goes into the material; 0 (default) = everything as before |
| `indexOfRefraction` | 1..3 | water 1.33, glass 1.5, diamond 2.42; default 1.5 |
| `attenuationColor` | 0..1 per channel | the colour light becomes after `attenuationDistanceMeters` inside (Beer–Lambert) |
| `attenuationDistanceMeters` | ≥ 0 | 0 = clear, no absorption |

Helpers: `material_has_optics`, `material_is_refractive` (transmission > 0) and
`material_optics_valid`.

## Older content stays byte-identical

Every format writes the optics only when a material has them. Hashes include them only then too.

- **DVOX**: v1.4 when a material has optics (v1.4 also carries the axis-scale header). Otherwise
  v1.2/v1.3 exactly as before. Readers accept v1.4.
- **DMESH**: v1.4 when a material has optics, otherwise v1.3. Content hashes of materials without
  optics are unchanged.
- **Game saves**: schema v5 adds an optional `dve.material_optics` section (object id, material
  index, optics). The v4 → v5 migration only bumps the version. A material whose optics were
  changed at runtime is no longer treated as "same as the source" and is saved inline.
- **Editor material library**: text format v4 (six more numbers per line); v1 to v3 still load.
- **Runtime scene, world state and brick-palette hashes**: optics are hashed only when present.

## Elsewhere

- **Validation**: DVOX, DMESH, the editor library, runtime scenes and game saves refuse optics
  out of range.
- **GPU**: `GpuMaterialRecord` / `MaterialRecord` grow from 23 to 29 dwords (116 bytes): transmission, IOR,
  attenuation colour and distance. No shader reads them yet; the caustic map will.
- **Master materials**: parameters `Transmission`, `IndexOfRefraction`, `AttenuationColor`,
  `AttenuationDistanceMeters` (also as runtime overrides, clamped, and readable from Lua).
- **Layers**: lerp blends all four; multiply and additive affect transmission (and multiply affects
  the colour); IOR follows the base in those modes.
- **glTF import**: reads `transmissionFactor`, `ior` (0 keeps 1.5) and `attenuationColor` /
  `attenuationDistance` (infinite = 0). The textures of those extensions are not used yet.

Not in this part: editing these in the editor's material panel, and rendering refraction.

## Tests

- `tests/test_material_optics.cpp`:
  - defaults and range checks;
  - DVOX and DMESH versions and round trips, with hashes unchanged for default optics;
  - editor library round trip and refusal;
  - glTF import of the three extensions;
  - the GPU record;
  - layer blending.
- `test_game_save.cpp`: the optics section appears only when needed, round-trips and re-encodes
  byte-identically; migration counts now go to v5.
- `test_gpu_material_buffer.cpp`: new size and offsets.
