# Lasso and paint selection (16 Oct 2026)

Artist worklist ART-018. Builds on the selection filters branch (masks, front-only boxes).

## Choosing the shape

Edit > Selection Shape: **Box**, **Lasso** or **Paint** (a radio group). **Shift+Q** cycles
through them; choosing one also switches to the Select tool. The status line names the shape and
its gestures.

## Lasso

Drag a freehand loop; it is drawn while you drag and closes back to its start. On release
everything inside it is selected in the active domain:

- **Objects**: anything visible inside the loop (X-Ray Selection off), or anything inside at all
  (X-Ray on). Closed groups count as one; the selection masks apply.
- **Voxels**: cells of the chosen object whose centres fall inside the loop, visible cells only
  unless X-Ray.
- **Mesh components**: vertices, edges or faces whose centres fall inside, with the same
  visibility rules as box selection (including the cage X-Ray for subdivision objects).

Shift adds, Ctrl removes, Ctrl+Shift intersects, as for boxes. A tiny loop is a click. The label
under the loop reads `Lasso · visible only` or `Lasso · X-Ray (through)`.

## Paint

Drag a round screen brush over things to add them; Ctrl-drag removes. The brush outline follows
the cursor; Ctrl+] and Ctrl+[ change its size (default 30 px). Dabs are spaced along the stroke,
so fast strokes leave no gaps.

- **Objects**: rays across the brush (centre plus two rings), nearest surface only unless X-Ray.
- **Mesh components**: components whose centres fall under the brush, visible ones unless X-Ray.
- **Voxels**: Paint is the existing voxel Selection Brush (its radius is the voxel brush radius).

## Also fixed

A closed lasso ends where it started, so it used to register as a click and the next press
became a double-click. Drags that drew a lasso no longer count as clicks.

## Tests

`tests/test_editor_selection_shapes.cpp`: shape setting and keys, lasso on objects (visible
only, X-Ray, Shift, Ctrl, masks, tiny loop as click, drawn loop), lasso on voxels and mesh
vertices, paint on mesh faces, objects (including Ctrl removal and the brush outline) and voxels.
