# Pivot editing for any object (19 Oct 2026)

Artist worklist ART-028. Builds on the typed-values branch.

An object's pivot is its origin. Moving it changes the object's position and shifts the geometry
the other way, so nothing moves in the world. Every change is one undo step and is saved with the
scene like any other edit.

- **Meshes and subdivision cages** move their origin exactly.
- **Voxel objects** keep whole cells, so their origin lands on the nearest voxel corner (on an
  exact tie the smaller step wins, so repeating a command changes nothing).

## Commands (Edit menu)

- **Edit Pivot**, previously groups only, now also works on one voxel object or mesh. The gizmo
  sits on the origin and Move drags only the pivot: the amber marker follows the drag and the
  geometry does not move until release. Hold V to snap the pivot to a vertex, edge, face or
  surface, the object's own included. Type a value for an exact offset (`1`, `25cm`, X/Y/Z to
  change axis). Esc cancels a drag; Edit Pivot again, Esc or selecting something else finishes.
  Rotate is refused while editing an object's pivot, because the origin keeps the object's
  rotation.
- **Pivot to Centre** and **Pivot to Bottom Centre** (renamed from Group Pivot to ...) work for
  groups as before, and now for voxel objects and meshes, several at once.
- **Pivot to Point Under Cursor**: the nearest snap target under the cursor, including the
  selection's own vertices and corners.

The pivot modes for transforms (Pivot at Selection Center / Active Object / World Origin /
Cursor Hit) are unchanged; with Pivot at Active Object, moves, rotations and V-snapping use the
edited origin.

## Refused, with the reason in the status line and the greyed-out menu

Locked objects, prefab instances, 3D text and volumes, imported exact-CAD meshes, and objects with
socket-attached children (they would move). Plain children keep their world positions.

## Tests

`tests/test_editor_pivot_edit.cpp`: Pivot to Bottom Centre on a mesh (geometry and child stay,
one undo), Pivot to Centre on voxels (corner, same cells, repeat is a no-op), several objects at
once, Pivot to Point on the object's own vertex, Edit Pivot (gizmo on the origin, nothing moves
while dragging, the marker, typed 1 m, one undo, V-snap to its own corner, Rotate refused,
selection change ends it), and refusals. The closed-groups test now expects Edit Pivot to work on
a single piece.
