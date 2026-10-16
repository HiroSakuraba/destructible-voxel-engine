# Selection basics (15 Oct 2026)

Artist worklist ART-010 to ART-014. One rule for adding and removing, keyboard walking through
the hierarchy and mesh, and a clearer pick list for stacked objects.

## One modifier rule (ART-012)

Everywhere you select (viewport clicks, box selection, the hierarchy panel, voxel cells and
pieces, mesh vertices / edges / faces, cage corners):

| Keys            | Effect                       |
|-----------------|------------------------------|
| none            | replace the selection        |
| Shift           | add                          |
| Ctrl            | remove                       |
| Ctrl+Shift      | keep only the overlap        |

Shift+click on something that is already selected takes it out again, so Shift alone is enough
to adjust a selection. The rule lives in one place, `selection_operation_for_modifiers()` in
`editor_native.hpp`. Before this, Ctrl toggled in some places and removed in others. Alt+drag
still removes in box selection where a profile leaves Alt free.

## Box selection (ART-010, ART-011)

Already in place and checked again: a box can start over geometry (the selection is decided on
release, so a drag from an object does not move it). Left-to-right boxes contain, right-to-left
boxes cross, and both test projected voxel cubes and mesh triangles, not just bounding boxes.

## Pick list (ART-013)

Clicking the same spot again still cycles through the objects under the cursor. Alt+click
without dragging now opens the depth-sorted list (Alt+drag still orbits; before, the default
profile's orbit binding swallowed Alt+click, so the list could not be opened). Each row reads

    Arm  (voxels, 64)  Rig / Arm

that is name, kind (voxels with cell count, mesh or subdivision with triangle count, 3D text,
Gabor volume, group) and the parent path. Pieces of a closed group answer for the group.

## Pick walking (ART-014)

The arrow keys in the viewport (and Alt+arrows) walk the selection. The camera never moves.

| Selecting | Up                  | Down                     | Left / Right                     |
|-----------|---------------------|--------------------------|----------------------------------|
| Objects   | parent              | first child              | previous / next sibling (wraps)  |
| Mesh      | grow one ring       | shrink one ring          | step to the neighbour that way on screen |
| Voxels    | grow                | shrink                   | (nothing)                        |

Hidden and isolated-out objects are skipped. Menu: Edit > Pick Walk Up / Down / Left / Right;
action ids `select.walk_up`, `select.walk_down`, `select.walk_left`, `select.walk_right`, bound
to the arrow keys in the Viewport context and rebindable in the shortcut editor.

## Tests

`tests/test_editor_selection_basics.cpp` (modifier rule, Shift-click removal, Ctrl-click, object
walking incl. wrap and roots, arrow and Alt+arrow keys, voxel and mesh walking, pick-list rows,
Alt+drag still orbiting).
