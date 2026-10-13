# Voxel region editing

Select a voxel object, press M, then select cells. The Voxel menu and Command Center provide:

| Operation | Menu choices | Grid behavior |
| --- | --- | --- |
| Extrude region | +X, -X, +Y, -Y, +Z, -Z, one cell per command | For each selected column, extend its furthest selected cell in the chosen direction. Retain the original region and select the new cells too. New cells inherit that frontier cell's material, without creating anchors. Repeated commands add layers. |
| Rotate region | +/-90 degrees around X, Y or Z | Exact right-handed integer rotation in the object's grid around a cell-centre pivot nearest the selected bounds centre; ties favor the minimum coordinate. Consecutive rotations reuse the pivot so four quarter turns restore even-width regions without drift. Changing selection or another edit starts a new pivot. |
| Scale region | x2, x3, x4 | Expand each selected cell into a factor-cubed block from the region's minimum grid corner. Materials and selected anchors expand with their source cell. Voxel metric and object transform stay the same. |

The build API additionally accepts signed extrusion distances 1..32, scaling factors 2..8,
and an explicit rotation cell-centre pivot. Scaling here enlarges cell geometry; it does not
resample or shrink a region.

All destinations are checked before a command exists. Extrusion refuses occupied destinations;
rotation and scaling may reuse selected source cells but refuse occupied unselected cells.
A blocked edit leaves geometry, anchors and selection unchanged. Empty/stale selections, locked,
hidden or non-voxel objects, integer coordinate overflow and outputs above 500,000 selected cells
are refused. Coordinates keep an eight-cell margin inside the signed 32-bit grid so brick
extents and neighbouring-cell queries remain representable. Material changes and anchors use the existing expected-state voxel patch command.
Each successful edit records one undo/redo transition for both geometry and cell selection.
There is no preview; each menu command applies one bounded edit immediately.

## Selection gestures

| Gesture | Operation |
| --- | --- |
| Click | Face-connected piece |
| Left-to-right box | Window: fully enclosed projected cells |
| Right-to-left box | Crossing: touching projected cell silhouettes |
| Shift | Add |
| Ctrl | Toggle |
| Ctrl+Shift | Intersect |
| Alt | Subtract when the navigation profile does not bind the gesture to orbit |
| Selection Brush | Explicit Select-tool mode: drag adds, Ctrl-drag removes, [ / ] changes radius |

Crossing uses a convex projected silhouette clipped at near/far depth planes and against the
selection rectangle, rather than a cell-centre or screen bounding-box test. Orthographic facing
uses the camera direction. X-Ray includes buried cells; without X-Ray the existing exposed-face
filter remains, without scene-occlusion testing. A selection still belongs to one object.
Oversized box results stop at the selection budget plus one; an oversized result is refused.
Ctrl toggle now computes the resulting set size correctly when toggling off a large selection.

## Manual handoff (not executed)

- Select an asymmetric, mixed-material region with an anchor. Extrude each signed axis; confirm
  retained sources, frontier materials, newly selected cells, unchanged original anchors, Undo/Redo.
- Rotate a 2-by-3 region four times around each axis. Confirm the initial cells, materials and
  anchors return exactly. Change selection and confirm the next rotation uses its new bounds.
- Scale an anchored two-cell region by 2, 3 and 4; confirm factor-cubed cell counts per source,
  material blocks, anchor blocks and the minimum-corner origin. Undo restores the original region.
- Put an unselected voxel in a destination for each operation; confirm the whole edit is refused.
  Also exercise locked/hidden objects, a stale selection, coordinate boundaries and output budget.
- Box partially covered cells in both directions; try off-screen corners, near-plane clipping,
  perspective/orthographic views and X-Ray. Exercise add/toggle/intersect, including deselection
  from a near-budget set; toggle brush mode and exercise add/remove/radius controls.
- Confirm menus and Command Center explain availability, Play prevents edits, other previews
  close on a successful region edit, and Undo/Redo restore selected cells as well as geometry.

Source review only. No builds, automated tests or CI were run, per the user's instruction.
The commit includes [skip ci].
