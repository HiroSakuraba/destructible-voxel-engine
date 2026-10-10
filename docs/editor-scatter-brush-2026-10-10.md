# Scatter Brush

The paint half of Scatter Objects (worklist ART-093; the fill half is in
[editor-scatter-2026-10-10.md](editor-scatter-2026-10-10.md)). Paint copies of objects onto
any visible voxel surface by dragging, erase them with Shift-drag.

## Workflow

1. Select what to paint: one or more objects in the scene, and/or a prefab in the Assets panel.
2. Pick **Scatter** in the toolbar (the last button). The sources are captured now; changing the
   selection afterwards does not change what the brush paints. To paint something else, select
   it and pick the tool again. With nothing selected the viewport hint says so.
3. In the viewport:

   | Gesture | Action |
   | --- | --- |
   | Drag | Paint copies under the brush |
   | Shift-drag | Erase scatter copies under the brush |
   | Ctrl+wheel | Radius ×1.15 per notch (0.25 m to 50 m) |
   | Ctrl-drag | Radius, continuously (drag right grows) |
   | Esc during a stroke | Cancel the stroke; nothing changes |

   The brush is a green ring lying on the ground under the pointer (red while erasing), labelled
   with its radius. While dragging, the spots the stroke will add are diamonds in each source's
   colour and the copies it will erase are red crosses. **The scene changes only when the
   button is released.**

## Rules

- **Undo:** each stroke (paint or erase) is one undo step.
- **Groups:** all paint strokes in one tool session go into one group, "Scatter (N)", tagged
  `dve.scatter`; N follows paint and erase strokes (unless you renamed the group). Picking the
  tool again starts a new group. If Undo removes the group, the next stroke makes a new one.
  The hierarchy no longer repeats the child count after a name that already ends with it.
- **Ground:** any visible voxel object, except the sources themselves, earlier scatter copies
  (copies never stack on copies) and objects hidden by View > Isolate Selection.
- **Spacing:** the brush uses the Scatter Objects spacing and Align-to-surface settings. New
  spots keep the spacing from each other and from every existing scatter copy, so repeated
  strokes over the same area fill gaps instead of piling up. A dab happens every 0.35 × radius
  of pointer travel.
- **Erase** removes direct children of any scatter group (fill or brush) whose footprint centre
  is inside the ring; locked copies and the original sources are never erased.
- Copies are independent objects, as with fill (prefab links are dropped).
- Layout is deterministic for a given seed and sequence of dabs.

## Code

- `plan_scatter_dab`, `append_scatter_copies`, `make_scatter_group`, `is_scatter_group`
  (`include/dve/editor_scatter.hpp`); fill now shares the copy and group code with the brush.
- Controller: `EditorToolId::ScatterBrush`, `scatter_brush_view()`, `set_scatter_brush_radius()`.
  Toolbar buttons now map through `kToolbarTools` (Scale still has no button), so tools can be
  added without matching enum order to button order.
- Headless screenshots: `dve_native_editor_x11 --drag x1,y1,x2,y2[,shift|ctrl][,hold]`
  (repeatable) drives primary-button drags and clicks after startup.

## Tests

`tests/test_editor_scatter_brush.cpp` (`dve_editor_scatter_brush_tests`): dab bounds, ground,
spacing (also against existing copies) and determinism; toolbar entry and hints; preview before
release; one undo step per stroke; one group per session with a live count; erase with undo;
Esc cancel; group re-created after undo; new group per session; brush ring drawn.
`test_editor_chrome_fit` and `test_editor_snap_scale` now check the toolbar through
`kToolbarTools`.
