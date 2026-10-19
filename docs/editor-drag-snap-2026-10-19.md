# Point snapping while moving (19 Oct 2026)

Artist worklist ART-025 and ART-026.

## Hold V while moving

With the Move tool, hold **V** and drag a Move gizmo handle. Instead of following the axis, the
selection's pivot jumps to the nearest target under the cursor, in this order of preference:

1. mesh vertex
2. voxel corner, or collision-box corner
3. edge midpoint
4. face centre (mesh triangle, or the voxel face under the cursor)
5. the object's bounds corner
6. the surface itself, lifted by the surface offset (Edit > Increase / Decrease Surface Offset)
   and, with Edit > Align Up to Surface Normal on, turned so the object's up follows the normal

Targets within 14 px of the cursor count; corners on an object's silhouette are found even when
the cursor is just off the object, and corners hidden behind geometry are not targets. The
viewport draws the target (a diamond with its name, such as `snap: vertex`), the pivot the drag
started from, and a line between them. The status line names the target and its object.

Release V to go back to the axis; Enter or releasing the mouse commits one undo step; Esc puts
everything back. It works for whole objects and for moving mesh components (snapping to other
objects). Selected objects are never targets.

## Placement modes

Edit gains **Snap to Mesh Vertex**, **Snap to Mesh Edge Midpoint** and **Snap to Mesh Face
Centre**, next to the existing voxel, bounds and collision-box modes: move over a target and
click to place. Placement modes now draw their target too.

## Already in place

Place on Surface with offset and Align Up to Surface Normal, and the voxel corner, edge, face,
bounds and collision-box placement modes, were already there; this adds mesh targets, snapping
during a drag, and showing the target before committing.

## Tests

`tests/test_editor_drag_snap.cpp`: vertex, edge midpoint, voxel face centre and corner during a
drag, the drawn label, releasing V, commit as one undo step, Esc, surface snapping, and the mesh
placement modes.
