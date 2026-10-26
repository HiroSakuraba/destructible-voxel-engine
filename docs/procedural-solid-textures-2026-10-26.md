# Procedural solid textures (26 Oct 2026)

Stacked on `feature/triplanar-displacement`. This change adds three procedural surface patterns
to the existing set (Wood, Leather, Paper, Mineral, Paint): Marble, Strata and Cellular stone.

They are **solid textures**: they are defined throughout the volume, not only on the surface.
When you cut, carve or blast an object, the pattern continues on the new faces.

Any surface pattern can now also serve as the **height source** for voxel displacement, so a
material can carve its own cracks or veins without a height map.

## The patterns

All three use the existing `MaterialSurfaceDetail` fields:

- **`periodMeters`**: the size of one band, layer or cell.
- **`rotationRadians`**: turns the whole field.
- **`seed`**: chooses the variation.
- **`colorVariation`, `roughnessVariation`, `bumpHeightMeters`**: act on colour, roughness and
  bump as for the other patterns.
- **`grainStretch`**: has a meaning of its own for each pattern, as follows.

| pattern | what it is | `grainStretch` |
| --- | --- | --- |
| Marble (6) | thin dark veins along planes of constant local x, bent by turbulence, over a faint mottle | vein sharpness: veins are (1 − \|sin\|)^(stretch / 4), thin when large (presets use 24) |
| Strata (7) | sedimentary layers of constant local y; each layer has its own hardness, so hard layers stand proud and soft ones recede; the layers undulate slowly | how far layers run before they undulate (stretch / 4 periods) |
| Cellular (8) | jittered cells (cobbles, flagstones, crazed stone): each stone is domed and has its own height, with flat cracks between stones | crack width: 1.6 / stretch of a cell |

**Neutral on average.** Each pattern is centred on its own mean, as the existing patterns are.
On average it leaves the base colour and roughness unchanged.

- **Marble.** The mean is integrated for the chosen vein sharpness.
- **Strata.** The mean is 0.
- **Cellular.** The mean is a fitted function of the crack width, within 0.002 for widths from
  0.005 to 0.5 of a cell.

**Filtering.** The detail fades to that neutral 0, with no bump, as a pixel's footprint outgrows
the finest feature:

- Marble: the vein width.
- Strata: the layer edge.
- Cellular: the crack.

Distant stone doesn't shimmer and doesn't drift in colour.

**Gradients.** Gradients (used for bump) are central differences of the value. They match the
value's change per metre, including rotation and period.

**Presets.** Three starter presets use the new patterns:

- "Polished marble": 25 cm veins.
- "Layered sandstone": 8 cm layers.
- "Cobblestone": 20 cm stones.

All three are in the "Stone" family.

**GPU.** `shaders/common/material_surface_detail.hlsli` implements the same three patterns, so
the live GPU material path draws them too. It compiles to valid SPIR-V. The CPU/GPU conformance
test (`test_gpu_material_surfaces`) now covers patterns 0 to 8, but it only runs where DXC has
SPIR-V codegen, and that isn't available on this machine. **It hasn't been run against these
patterns yet.**

## Patterns as height sources

A voxel material displaces when it has a displacement depth and either a height map or a surface
pattern. This uses the displacement fields from the previous branch, `displacementDepthMeters`
and `displacementMode`.

- **Height map first.** The height map is used when there is one.
- **Pattern otherwise.** Otherwise the pattern gives the height, as h = 0.5 + 0.5 × value
  (`surface_pattern_height`). The pattern must be in Object or World space; on voxels it is read
  in object space.
- **One height field per face.** Heights are read on each face's plane, so a face is one height
  field. Traced displacement and parallax therefore see the same surface, and the parallax
  fallback still matches traced displacement exactly.
- **Colour follows the parallax point.** When a face is drawn with parallax, the pattern's
  colour and roughness are read at the parallax point too. Cracks recede in colour as well as in
  shape.
- **Meshes made from voxels.** They get the same parallax.

A cobblestone floor with a displacement depth of 5 cm has real cracks:

- The depth pass varies by 4 cm across the stones.
- The cracks shadow themselves.
- Rays pass through them.

## Saving

There are no new fields. Files that use a new pattern are written at the next version, so older
readers refuse them instead of misreading them:

| where | version | older versions |
| --- | --- | --- |
| DVOX | 1.12 (only when a material uses Marble, Strata or Cellular) | a 1.11 file claiming one is refused |
| DMESH | 1.9 (likewise) | a pre-1.9 file claiming one is refused |
| Editor material library | 12 (likewise) | a pre-12 file claiming one is refused |
| Game saves | unchanged: the `dve.material_surface_detail` section validates the pattern range | |

## Checks

`dve_solid_textures_tests`:

**Statistics.** Each pattern spans about two units and averages to 0 over 20,000 points:

| pattern | mean |
| --- | --- |
| Marble | 0.005 |
| Strata | 0.038 |
| Cellular | −0.002 |

At a footprint of 100 periods, each filters to exactly 0, with no gradient. Pattern 9 is invalid.

**Gradients.** At 90% of 200 random points, the gradient matches finite differences of the value
under a rotated field. The rest are kinks: vein crests and cell borders.

**Character**
- **Marble.** The vein share falls from 0.31 to 0.02 as `grainStretch` goes from 4 to 80.
- **Strata.** Over 2 cm, the value changes 3.6 times more across the layers than along them.
- **Cellular.** The crack share grows with crack width (0.112 wide, 0.024 narrow), and the stone
  varies equally along all three axes.

**Height source**
- **Traced.** A cellular voxel with no height map carves to exactly (1 − h) × depth at 100 points
  (worst error 0.00002 cells). Cracks go 0.4 cells deep and stones 0.06.
- **Parallax.** It meets the same depth straight on and shifts the read point when the view is
  oblique.

**Render.** A 5 cm cobble floor spreads the depth pass by 4 cm (0.5 mm when flat) and has 131
pixels in crack shadow.

**Formats.** DVOX 1.12 round-trip; DVOX 1.11 refuses a solid pattern; older patterns keep
DVOX 1.7; DMESH 1.9 round-trip.

Other tests that changed:

- `test_material_optics` round-trips the editor library at version 12, with an earlier
  displaced material still intact.
- `test_material_surfaces` runs its derivative, filtering, seed and large-coordinate checks on
  all eight patterns.
- `test_editor_material_presets` expects 44 starter presets.

## Not in this change

- **Mesh displacement on patterns.** `build_displaced_polygon_asset` still reads only height
  textures and height maps.
- **Rule-driven masks.** Slope, height, cavity, edge wear and weathering masks for the layer
  compositor are the next branch.
