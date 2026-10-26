# Editor and Lua controls for texturing (26 Oct 2026)

Stacked on `feature/persistent-damage-marks`. The texturing branches added texture sets,
displacement, solid patterns, rule-driven layers, weathering and damage marks, but only C++
could set them. This branch adds the editor and Lua controls.

## Editor

**Material panel.** Five new properties edit the draft like any other slider and are applied
with Apply / Edit slot:

| control | field | range |
| --- | --- | --- |
| Texture repeat | `textureTileMeters` | 0.0001 … 10000 m |
| Texture blend (advanced) | `textureSharpness` | 1 … 64 |
| Anti-tiling | `textureAntiTiling` | 0 … 0.5 tiles |
| Displacement depth | `displacementDepthMeters` | 0 … 10 m (drawn capped at half a voxel) |
| Displacement mode | `displacementMode` | 0 traced, 1 parallax (whole numbers) |

In simple mode, the texture-set controls appear only when the material has a texture set, and
the displacement controls only when it has a height source (a height map or a surface pattern).
Advanced mode shows them all. The properties are numbered after the existing ones, so preset
override bits and saved presets keep their meaning.

**Weathering.**
- **The command.** Materials > Add Weathering to Selection (`material.weather`) adds the panel's
  current recipe at its current amount. Materials > Next Weathering Kind (`material.weather_kind`)
  cycles Snow → Moss → Rust → Dust → Wetness → Edge wear.
- **Voxel objects.** Every material the selected voxels use (or the whole object without a voxel
  selection) gains a rule-driven layer. One new palette slot holds the recipe's source material,
  shared by those layers. Library materials are copied into the object's palette, so other
  objects are unaffected.
- **Polygon objects.** The panel's slot gains an overlay, with the source material and an
  empty binding.
- **Seeds.** Each object's seed follows from its id and the recipe kind.
- **Undo and saving.** It is one undo step. Scenes save it: palettes are written as DVOX 1.13,
  which keeps the rules.

**Layers mode** (polygon slots), new rows:
- **Weathering.** Weathering kind (cycles), Weathering amount (typed) and Add weathering overlay.
- **Rules.** Rules on/off for the selected overlay. When on: Faces up at least (cos), Above
  height, Below height, Base height bias, Noise breakup, Noise scale and Edge softness, each typed
  and validated (an out-of-range value is refused and nothing changes).

## Lua

Material slots are material ids, as in `raycast` hits; layers are one-based, as in
`set_material_layer_weight`. These functions work on objects that carry a material table:
scene objects and `spawn_asset`. Boxes from `spawn_box` have none. Failures return `nil` and
a message, and change nothing.

| function | does |
| --- | --- |
| `world.get_material(id, material)` | a table: name, `displacement_depth`, `displacement_mode` (`"traced"`/`"parallax"`), `texture_tile`, `texture_sharpness`, `anti_tiling`, `has_texture_set`, `pattern` and its `pattern_*` settings, and `layers` (source, weight, enabled, blend, `rules` when on) |
| `world.set_material_texturing(id, material, {...})` | any of `displacement_depth`, `displacement_mode`, `texture_tile`, `texture_sharpness`, `anti_tiling`, `pattern` (`"none"` … `"cellular"`), `pattern_space` (`"object"`/`"world"`), `pattern_period`, `pattern_stretch`, `pattern_seed`, `pattern_color_variation`, `pattern_roughness_variation`, `pattern_bump`; all or nothing |
| `world.add_weathering(id, material, kind, amount [, {seed=, up=}])` | adds a recipe (`"snow"`, `"moss"`, `"rust"`, `"dust"`, `"wetness"`, `"edge_wear"`); returns the layer number |
| `world.set_layer(id, material, layer, {weight=, enabled=, blend=, rules=})` | `rules` is a table (only the named fields change; it turns the rules on) or `false` (off). Fields: `up`, `slope_min`, `slope_max`, `height_min`, `height_max`, `occlusion_min`, `occlusion_max`, `edge_width`, `base_height`, `noise`, `noise_scale`, `softness`, `height_softness`, `seed` |
| `world.remove_layer(id, material, layer)` | removes a layer (its source slot stays) |
| `world.set_damage_marks({enabled=, exposure=, scorch=, cracks=, reach=, crack_reach=, max=})` | damage-mark settings |
| `world.get_damage_marks([id])` | the settings and the marks (`id`, object-local `center`, `radius`) |
| `world.add_damage_mark(id, x, y, z, radius)` | stamps a crater mark without carving |
| `world.clear_damage_marks([id])` | clears all marks, or one object's |

The Lua bindings are written for a Lua built as C: they don't call `luaL_check*` or `luaL_error`
once a C++ object with a destructor exists, so an error can't longjmp past one.

**Behind it,** `GameWorld` has new material-table methods:

- `object_materials(id)` reads an object's table.
- `set_object_material(id, slot, material)` keeps the slot's physical fields (density, strength,
  fracture resistance, flammability, conduction, structural, transparency), so mass and
  destruction don't change. It validates the whole table: the formats' range checks, layer references
  and no cycles.
- `add_object_material(id, material)` appends a slot that no voxel uses.

Edited tables travel with fragments and are saved inline.

**Fix.** `GameWorld::state_hash` now also covers texture sets, displacement, layer rules and
anti-tiling, but only when they are used, so older hashes keep their values. Before this, a world
whose materials differed only in those fields hashed the same.

## Checks

- **`test_editor_material_presets`, `texturing_controls`:**
  - **The properties.** They are set and read back. A fractional mode and anti-tiling above 0.5
    are refused.
  - **The simple panel.** It shows the displacement controls with a height source and hides them
    without one, and it hides the texture-set controls without a texture set.
  - **Moss on a voxel object.** It gives both used materials one layer from one shared Moss slot,
    with rules. It is one undo step, the saved scene keeps the rules and seed, undo removes it,
    and an amount above 1 is refused.
  - **Polygon slot.** From the Layers rows: next kind, amount, then add; the overlay and binding
    appear. The Rules rows are listed, a rule edit applies, an out-of-range rule is refused, and
    the rules turn off.
- **`test_game_script`, `test_texturing_controls_from_lua`:**
  - Reading a material works, and an unknown slot gives `nil`.
  - A texturing change applies, and a change with an unknown pattern is refused whole: its
    depth didn't apply.
  - Snow weathering comes back as layer 1, and the rule and weight edits read back.
  - Out-of-range rules and unknown kinds are refused, and rules turn off.
  - The damage-mark settings apply, a carve leaves 1 mark and a stamp adds another, bad settings
    are refused, and clearing works.
  - In C++: density is kept, the Snow slot is appended, and a layer cycle is refused.

**Lua tests in this build.** These tests ran with `DVE_ENABLE_LUA=ON`, using the system
Lua 5.4. That turned up two older fixture problems from the atmosphere branches, which never
ran without Lua:

- **`test_game_script`.** Its synthetic version-4 script state removed only the lighting
  extension, so the version 6 sky block and the version 7 fog block that follow it were read as
  HUD data.
- **`test_game_save`.** Its version-3 fog fixture cut a fixed tail that no longer included those
  two blocks.

Both fixtures now remove the blocks; the loader itself was right. With Lua on, all 372 tests
pass.

## Not in this change

- **Editor voxel rules.** The editor has no per-layer rule editing for voxel palettes; weathering
  adds them and Lua edits them.
- **Map import.** There is no editor UI for importing texture-set maps.
- **Spawned boxes.** Objects from `spawn_box` carry no material table to edit.
