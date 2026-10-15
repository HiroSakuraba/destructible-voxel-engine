# Duplicate Special

Make many copies of the selection in one go, each offset, turned and scaled from the one
before. For example: duplicate a feather, move it 3 inches, turn it 5 degrees, and make 200.

## Using it

**Edit > Duplicate Special...** (Ctrl+Shift+D) opens a panel in place of the inspector details.
A ghost box for every copy shows in the viewport (up to 1,500 boxes, the last one brighter).
Nothing changes until **Apply** (Enter, or Ctrl+Shift+D again), which makes the copies as **one
undo step**. **Cancel** (Esc) closes the panel.

| Row | Meaning |
| --- | --- |
| Copies | 1–2000 (and at most 20,000 objects in all, counting children) |
| Offset X / Y / Z | Distance per copy: `3in`, `10cm`, `0.1` (meters) |
| Turn X / Y / Z | Rotation per copy in degrees (`5`, `0.25turn`, `(pi/2)rad`) |
| Scale | Size multiplier per copy (`0.98` shrinks each copy 2%). Voxel objects scale by voxel size, polygon and subdivision meshes by axis scale; 3D text and Gabor volumes keep their size. |
| Pivot: Each copy | Each copy steps from the previous one about its own origin. **Offset in Local** follows the turn, so offset plus turn makes an arc or coil. **World** moves along world axes. |
| Pivot: Around point | Copy *k* is the source turned *k* times around the point. A turn alone makes a ring or fan; an offset along the turn axis makes a spiral. **Set point** puts the point at the world origin or the centre of the selection; the Point X / Y / Z boxes take any value. |
| Put copies in one group | On by default: the copies go under a new group, "Feather x200", which is selected. Off: the copies sit beside the source, and all are selected. |

The numbers work like the inspector fields (numeric fields, ART-085):

- **Typing:** units, arithmetic and `+= -= *= /=`, with Tab / Shift+Tab between boxes.
- **Scrubbing:** drag a row name or axis letter. Copies change by 1 per 6 px, scale by ×1.002 per
  px. Shift is finer, Ctrl snaps.

The settings are kept between uses.

Copies are named after the source with a number ("Feather 001" … "Feather 200"). Children come
along, keep their place on the copy, and are re-parented to it. The source is left where it is.

## Refusals and closing

- **Refused:**
  - nothing selected;
  - a hidden or isolated-out object;
  - the Voxels or Polygons selection domain;
  - Play or Simulate;
  - a long job running;
  - more copies or objects than the limits;
  - a scale of 0;
  - a scale that would make the last copy over 1000× larger or smaller.

  Each refusal says why. The summary under the buttons turns red with the reason.
- **Closing:** the panel closes when a source object is deleted, Play starts, or the selection
  domain changes.

## Code

- `include/dve/editor_duplicate_special.hpp` / `src/editor_duplicate_special.cpp`:
  `DuplicateSpecialSettings`, `duplicate_special_poses` (the pose of each copy) and
  `duplicate_special_problem`.
- Controller:
  - `open_duplicate_special`, `apply_duplicate_special`, `cancel_duplicate_special`;
  - `duplicate_special_preview` (oriented boxes), `duplicate_panel_layout`;
  - new `NumberFieldId`s (`DuplicateCopies`, `Offset`, `Rotation`, `Scale`, `Point`) and
    `TextEditKind::DuplicateSetting`, so typing, scrubbing and Tab reuse the numeric-field code.
- Apply builds one `CompoundCommand`: the group, then every copy as `AddObjectCommand`. The
  selection change is recorded with the command.
- Renderer: the panel and the ghost boxes.

## Tests

`tests/test_editor_duplicate_special.cpp` (`dve_editor_duplicate_special_tests`):

- **Poses:**
  - 72 local steps of 0.1 m and 5° close a circle;
  - world offsets go in a straight line;
  - around a point with a step up makes a spiral that comes back round;
  - scale compounds;
  - the limits.
- **Feathers:** 200 copies at 3 in and 5°, typed with units. The preview shows 400 boxes (feather
  and tip) and the panel is drawn. Enter makes a group of 200 with the tips in place, and one undo
  removes all of it.
- **Around a point:** Origin, a quarter-turn ring, halved voxel size, the group turned off.
- **Refusals:** nothing selected, hidden objects, scrubbing Copies, the 2000 limit message, Esc,
  and the domain change closing the panel.

## What's not done

- Linked copies (instances that share the source's geometry): copies are independent.
- Random jitter per copy (that is Scatter).
- Copying along a curve or path.
- The ghost is a box per object, not the actual voxels.
