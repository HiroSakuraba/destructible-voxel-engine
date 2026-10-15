# Numeric fields: type, scrub, reset

Artist worklist ART-085 (editable numeric fields with units) and ART-084 (multi-selection
editing).

## Where the fields are

| Field | Where | Unit |
| --- | --- | --- |
| Position X / Y / Z | Inspector, one box per axis | meters |
| Rotation X / Y / Z | Inspector, one box per axis | degrees (XYZ Euler) |
| Voxel size | Inspector, under Rotation, for voxel objects | meters |
| Brush radius | Viewport tool strip, under the hint, with Add / Remove / Paint or the voxel Selection Brush | voxels (0–32) |
| Surface offset | Viewport tool strip, with Select / Move / Rotate (objects) or while placing on a surface | meters (±10) |

The axis letters are coloured X red, Y green, Z blue. In a narrow inspector (< 320 px) the boxes
sit on the line under the row name.

## Typing

Click a box (or the row name, which opens X) and type, then press **Enter** to apply. **Tab** and
**Shift+Tab** apply the current box and move through Position X/Y/Z, Rotation X/Y/Z and Voxel
size. **Esc** cancels, and clicking elsewhere applies.

Values accept:

- **Units:**
  - lengths: `150cm`, `25 mm`, `2 ft`, `6in`, `1'6"`, `1.2km`;
  - angles: `90deg`, `0.25turn`, `(pi/2)rad`.

  A bare number is in the field's unit. A wrong kind of unit is refused with the reason ("Angles
  are not allowed here").
- **Arithmetic:** `2*0.25`, `(1+2)/4`, `1m + 50cm`, `1e-2`.
- **Relative edits:** `+=0.5`, `-=10cm`, `*=2`, `/=2` change the current value.
- **Three values** at once in Position or Rotation: `1 2 3` or `1, 2, 3`.

Invalid input changes nothing and says why in the status bar.

## Several objects (ART-084)

- A typed value **sets each selected object's value** for that axis. Other axes are untouched, and
  the whole change is one undo step ("Set Position X").
- Relative edits apply to each object, so `+=1` moves all of them by 1 m and keeps their spacing.
- Rotation turns each object about its own origin.
- An axis where the objects differ shows **—**. Its box opens empty, so typing sets them all to
  the same value.
- The name line says "Edits set each object".

This replaces the old rule, where typing moved the whole selection by the primary object's
change. `inspector_selection` tests now check the new rule.

## Scrubbing

Drag an axis letter (or the Voxel size / Brush radius / Surface offset label) left or right:

| Field | Per pixel | Ctrl snaps to |
| --- | --- | --- |
| Position, Surface offset | 0.01 m | The move snap step (offset: 0.05 m) |
| Rotation | 0.5° | The rotate snap step |
| Voxel size | ×1.01 | 5 mm |
| Brush radius | 1 voxel per 8 px | — |

**Shift** scrubs ten times finer. The scene updates live; releasing applies it as **one undo
step** ("Scrub Position X"), and **Esc** puts everything back. Pressing a label without dragging
types into that field. A locked object refuses the scrub.

## Right-click

Right-clicking any field offers:

- **Reset**: Position and Rotation to 0, Voxel size to 0.1 m (the engine default), Brush radius
  and Surface offset to 0;
- **Reset Position / Rotation X, Y, Z**: one undo step;
- **Copy** the value;
- **Paste** it into any field. Copying an axis where the objects differ is refused.

## Code

- `include/dve/editor_number_field.hpp` / `src/editor_number_field.cpp`: `parse_number_field` (a
  small recursive-descent parser for units, arithmetic and relative edits) and
  `format_number_field`.
- Controller:
  - `NumberFieldId`, `NumberFieldRect`;
  - `number_field_rects()`, computed on demand from the inspector rows and the viewport, so a
    selection or tool change never needs a relayout;
  - `number_field_available`, `number_field_text`, `set_number_field`, `reset_number_field`;
  - the scrub state; the `inspector.field.*` context-menu actions;
  - Tab order.
- Voxel size uses `ScaleVoxelSizeCommand`, and transforms use `TransformObjectsCommand`.
- The inspector keeps a third row (`inspectorFields[2]`) for Voxel size for any primary that is not
  3D text or a Gabor volume. When the primary has no voxels, that row shows the old "Selection / Axes"
  line, and Axes also moves to the ID line.
- Renderer: the per-axis boxes, the dash, the editing highlight and the tool strip.

## Tests

`tests/test_editor_number_fields.cpp` (`dve_editor_number_fields_tests`):

- **Parser:** units, feet-inches, arithmetic, relative edits, angles, refusals, formatting.
- **Typing:** per-axis typing with an undo label; the dash; setting both objects; `+=`; Rotation
  in radians; Tab and Shift+Tab; wrong units; locked objects.
- **Voxel size:** typed in cm with one undo; out of range is refused; no field without voxels.
- **Scrub:** live, then one undo; Shift fine; Ctrl snap; Esc; a click on the letter types; voxel
  size.
- **Context menu:** Reset X, Copy Y, Paste onto Z, Reset row as one undo.
- **Tool strip:** Brush radius for Add (typed, capped at 32, scrubbed); Surface offset for Select
  (typed in cm, reset).

Updated:

- `test_editor_inspector_selection.cpp`: typed values set each object; rotation in place; the
  pivot no longer matters.
- `test_editor_authoring_gaps.cpp`: pressing the axis letter starts a scrub, so the test releases
  before checking.

## What's not done

- Scale fields (left out by choice; the scale gizmo is ART-027).
- Unit display preferences (always meters and degrees), and a units setting for imperial display.
- Mouse-wheel nudging of fields, and dragging inside the box itself (only the label scrubs).
- Fields for 3D text and Gabor volumes keep their single-line editing.
- Brush shape (on the brush-preview branch) is not in the tool strip yet.
