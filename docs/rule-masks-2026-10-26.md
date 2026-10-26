# Rule-driven layer masks and texture anti-tiling (26 Oct 2026)

Stacked on `feature/procedural-solid-textures`.

## Layers on voxels

Main already had material layers: up to four per material (`VoxelMaterialLayer`), each blending
a source material over the base. The CPU voxel renderer ignored them, while the GPU material
table flattened them by their uniform weight.

The CPU voxel reference renderer now composites layers at each hit. Each enabled layer blends
its source material by weight × its rule factor, after the base's maps, surface pattern and
parallax.

- **Without rules.** Coverage is the plain weight, which now matches what the GPU shows.
- **What a layer changes.** The blend changes the scalar properties (`blend_material_layer`):
  colour, roughness, metallic, emission, specular, coats and so on. The normal and the base's
  maps are kept.
- **Nested layers.** A source material's own layers are not applied on top.
- **Stats.** `VoxelReferenceRenderStats::layeredSurfaces` counts the pixels a layer covered.

## Rules

`VoxelMaterialLayer::rules` (`dve/material_layer_rules.hpp`) says where a layer goes, from the
geometry instead of a painted mask. Each rule is a soft band with its transition centred on the
threshold, so coverage is exactly 0.5 at the threshold. The factors multiply.

| rule | fields | e.g. |
| --- | --- | --- |
| slope | `up`, `slopeMinCos` … `slopeMaxCos` (cos of the angle to up), `softness` | snow: `slopeMinCos` 0.7 |
| height | `heightMinMeters` … `heightMaxMeters` along `up`, `heightSoftnessMeters` | snow line, dirt near the ground, a tide mark |
| occlusion | `occlusionMin` … `occlusionMax` of the ambient occlusion | dirt or moss in hollows: `occlusionMax` 0.75 |
| edge | `edgeWidthMeters` | wear, chipped paint, frost along exposed convex edges |

Two further terms are summed as s and scale the band: coverage = clamp(factor × (1 + s)). They
break up and bias the band but never spread outside it.

| term | field | effect |
| --- | --- | --- |
| base height | `baseHeightInfluence` −1 … 1 | adds influence × (1 − 2h), with h the base's height (its height map, else its surface pattern). Positive values fill low parts first: water, sand and dirt in the cracks of cobbles. Negative values favour high parts: wear on the stones' tops. |
| noise | `noiseAmount`, `noiseScaleMeters`, `seed` | ragged, natural edges (filtered by the pixel footprint) |

**Edges are voxel-exact.** Each edge is found from the hit cell's exposed faces: an edge is where
two exposed faces on different axes meet. Its distance is the analytic distance to that edge.

Two limits follow from working per cell:

- **Width.** `edgeWidthMeters` is effectively capped at one voxel.
- **Meshes.** Surfaces without edge information get no edge coverage.

**Meshes.** On meshes, the rules multiply the layer's mask in the polygon compositor:

- Slope, height, base height (the compositor's base height) and noise apply.
- AO counts as fully open (1).
- Edge coverage is 0.

## Anti-tiling

`VoxelMaterialDefinition::textureAntiTiling` ranges from 0 (off) to 0.5:

- **Random offsets.** Every tile of each triplanar projection is offset at random.
- **Blending.** Neighbouring tiles blend across a band of that width, in tiles, centred on their
  shared edge.
- **Cost.** Inside a tile it reads one sample; near edges it reads up to four.

**Alignment.** All texture-set maps share the offsets, so colour, ORM, emission and height stay
aligned. Traced displacement and parallax read the same anti-tiled height. The normal map, which
has its own tiling, is unchanged.

## Saving

Rules and anti-tiling are written only when used:

| where | how |
| --- | --- |
| DVOX | 1.13: anti-tiling, then per layer a flag and, when set, the seed and 15 values |
| DMESH | 1.10: the same |
| Game saves | section `dve.material_rules`: object id, material index, anti-tiling, layer count, then per layer the flag, seed and values |
| Editor material library | version 13 |
| Hashes | runtime scene, brick palette, polygon and game-save material hashes include `material_rules_hash` |

## Checks

`dve_layer_rules_tests` reads coverage exactly from the emission pass: the base emits blue and
the layer red, with no other light.

**The factor**
- Disabled rules give 1.
- Slope, height and occlusion are exactly 0.5 at their thresholds.
- The up vector sets height.
- Edges give 0 without information, 1 on the edge and 0.5 halfway.
- Base height and noise scale the band but never leave it.
- Empty bands and a zero up vector are invalid.

**Edge distances.** Corner distance, a single face and opposite faces.

**On voxels**
- **Uniform layer.** A uniform 0.3 layer covers every pixel by exactly 0.3.
- **Snow.** It covers all up-facing pixels fully and no wall.
- **Height band.** It picks the raised block exactly.
- **Edge wear.** It matches the analytic distance to the block's top edges at 7,056 pixels, with
  error 0.00000.
- **Cavity dirt.** It covers 0.82 at the foot of the block and 0.00 on open floor (32 AO
  samples).
- **Base-height fill on a cellular floor.** It matches 0.5 × clamp(2 − 2h) at 4,096 pixels, with
  error 0.0000.

**Anti-tiling**
- Off, it is the plain sample.
- Tile centres differ between tiles (the test requires at least 6 of 8).
- It is continuous across tile edges: the largest step is 0.0056, against 0.0031 for the plain
  repeat.
- ORM and height stay aligned.
- Traced displacement reads the anti-tiled height.
- Values above 0.5 are invalid.

**Formats**
- DVOX 1.13 and DMESH 1.10 round-trip, and the rules change the content hash.
- `test_game_save` round-trips the rules and anti-tiling in `dve.material_rules`.
- `test_material_optics` round-trips the editor library at version 13.

## Not in this change

- **GPU.** The GPU material table still flattens layers by their uniform weight; rules and
  anti-tiling are CPU reference only.
- **Path tracer and radiance-cascade GI.** Secondary hits use the base material without layers.
- **Editor UI and Lua.** There is no UI or Lua for the rules yet.
