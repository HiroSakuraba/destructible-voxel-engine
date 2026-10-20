# Export Exact CAD (STEP) (20 Oct 2026)

Exact CAD, first batch, part 4. Builds on Measure and exact snapping.

## File > Export Exact CAD (STEP)...

Writes the selected CAD objects, and the CAD pieces of selected groups, into one STEP file in the
project's `exports/` folder: `exports/<object or group name>.step`. Each object becomes a named
solid placed where it sits in the scene, built from its exact source, not from its triangles.

A short list picks the format:

| Entry | Use |
|---|---|
| Millimetres, Z up | most CAD programs |
| Millimetres, Y up | |
| Metres, Z up | |
| Metres, Y up | as in this editor |

The export runs in the background. The status line then reads, for example:

`Exported 2 CAD parts to exports/Bracket.step (mm, Z up), read back and checked; stretched, exported converted (not exact): Plate; skipped, no exact source: Lamp`

- **Checked.** brepkernel checks every placed part and reads the written file back (same number
  of solids, same total volume) before it replaces anything. If that fails, no file is written and
  an existing file with that name is left as it was.
- **Stretched objects** (different scale per axis) are exported converted (Ben's choice): the
  shape is still a valid solid at the right size, but a stretched cylinder is no longer a cylinder,
  so its surfaces are stored as freeform (NURBS). The status line names these parts.
- **Not CAD.** Selected objects without an exact source (plain meshes, voxels) are skipped and
  named. With no CAD object selected, the menu item is disabled and says why.
- Moved, turned and uniformly scaled objects stay exact.
- An installed brepkernel without the `export` command gets a clear "update brepkernel" message.

## Code

- `export_exact_cad_step` and `exact_world_placement` in `editor_exact_cad.hpp`: writes the parts
  list (`{"parts":[{"path","name","transform"}]}`, 3x4 world transform in metres) and runs
  `brepkernel export --parts … -o … --units mm|m --up z|y`.
- `src/editor_native_cad_step.cpp`: the menu item, the format list, the background job and the
  status line.

## Tests

`tests/test_editor_cad_step_export.cpp`: world placements (rotation, scale, translation); the parts
list (names, sources, transforms); units and up axis; the report; refusals (nothing selected,
wrong extension, missing source, older brepkernel); the native command (disabled state, format
list, skipped and converted parts named, failure leaves no file). With the real brepkernel: a
12 mm pin and a stretched one in mm, Z up: volumes, names in the file, read-back error about 1e-14,
and a failed export that keeps the earlier file.
