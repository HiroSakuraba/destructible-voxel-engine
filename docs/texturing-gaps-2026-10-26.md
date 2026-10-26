# Texturing gaps: voxel overlay rules, map import, tables for spawned boxes (26 Oct 2026)

Stacked on `feature/texturing-controls`. This closes the three gaps that branch listed.

## Editing voxel overlay rules in the editor

Layers mode used to edit polygon slots only. With voxel objects selected, it now edits the
overlays of material **slot** (the panel's slot number, a voxel material id) in each object's
palette. A library material is copied into the palette, so other objects are unaffected.

- **Rows.** Select overlay, Mute/Enable, Remove, Weight, Color blend, Rules on/off, and the rule
  fields.
- **Voxel-only rules.** Voxel overlays also get "In hollows (AO at most)" and "Along exposed
  edges within", the two rules that need voxel AO and edges.
- **Validation and undo.** Each edit is validated and is one undo step. An out-of-range value
  is refused and changes nothing.
- **Shared code.** Polygon and voxel rows share the same rule editor (`edit_layer_rule`,
  `add_rule_rows`).

## Importing texture-set maps in the editor

- **Menu.** Materials > Import Base Colour / ORM / Height / Emission Map reads the PNG or JPEG
  selected in the Assets panel into the material panel's draft. Base colour and emission are read
  as colour, ORM and height as data. Materials > Clear Texture Maps removes them.
- **Applying.** Apply then assigns the draft, with its maps, to the selected voxels, like any
  other draft change. Once a map is present, the texture-set sliders (repeat, anti-tiling) appear
  in simple mode, and a height map brings up the displacement sliders.
- **Saving.** Scenes save the maps with the palette (DVOX 1.10 and later).
- **Meshes.** Apply refuses polygon targets when the draft has maps. DMESH doesn't store
  texture sets, and meshes have their own texture bindings, so the maps would be lost on save.
- **API.** `NativeEditorController::material_panel_import_map(slot, path)` and
  `material_panel_clear_maps()`.

## Material tables for spawned boxes

Objects made by `create_object` or `world.spawn_box` have no material table; the CPU player
draws them with a default colour per material id.

- **`default_voxel_material(id)`** is that look, now in core. The player uses it too, so nothing
  changes on screen.
- **`GameWorld::ensure_object_material_table(id)`** creates the table on demand: one default
  entry for every id the voxels use. Mass is untouched, since the mass table was built at
  creation.
- **When it happens.** `set_object_material` and `add_object_material` call it first, so every
  Lua texturing function works on spawned boxes. `world.get_material` on such an object reports
  the default look without creating anything.

## Checks

**`test_editor_material_presets`, `texturing_controls`**
- **Voxel overlay rows.** After moss is added, the material's overlay rows include the AO and
  edge rules.
- **Rule edits.** An edge-width edit applies to the palette as one undo step, an AO of 2 is
  refused, the weight changes, and two undos restore it.
- **Map import.**
  - A missing image is refused.
  - A generated 4 × 4 PNG imports as height (data role) and as base colour (colour role).
  - Apply refuses the mesh, then assigns the maps and a 3 cm displacement depth to the voxels.
  - Clear removes the maps from the draft.

**`test_game_script`**
- A `spawn_box` reads its default material.
- `add_weathering` returns layer 1, and a marble pattern applies.
- The created table has the three entries (air, box, Moss), with marble on the box material.

Full suite with Lua on: all 372 tests pass.
