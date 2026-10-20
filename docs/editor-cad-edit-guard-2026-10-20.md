# Ask before editing the triangles of a CAD object (20 Oct 2026)

Exact CAD, first batch, part 2. Builds on CAD Detail.

A CAD object's triangles are cut from its exact B-rep. Until now nothing stopped a bevel, a
Manifold Boolean or a vertex drag from changing those triangles while the exact source stayed
as it was. Exact Booleans, CAD Detail, and later measuring and STEP export would then quietly use
the old shape.

## What happens now

An edit that would change a CAD object's triangles asks first:

> Editing triangles drops the exact CAD link of Bracket (it becomes a plain mesh).
> Enter continues, Escape cancels. Undo brings the link back.

- **Enter** switches the object's exact link off (one undo step, "Drop exact CAD link"), then runs
  the edit (its own undo step). Undo once removes the edit, Undo again restores the link.
- **Escape** changes nothing: "Cancelled: the CAD link is kept".
- A gizmo drag or typed move of vertices, edges or faces asks when the drag starts; after Enter
  the status says to drag again (the drag itself is not replayed).
- The inspector shows `Was CAD: exact link dropped (Undo restores it)` for such an object.
- With Settings > Confirm destructive actions switched off, the link drops and the edit runs
  without asking.
- Locked objects are not asked about: the edit refuses on its own, so a link is never dropped
  for an edit that does nothing.

The exact source file is not deleted; the component stays on the object, switched off. Switching
it back on in the components panel restores the link, but only Undo puts the matching triangles
back.

## Which edits ask

Asks: Bevel, Extrude, Inset, Recalculate/Flip Normals, Weld, Dissolve, Loop Cut, Clean Unused,
Extract, Slice, Join, Separate, the Manifold Booleans (`mesh.boolean_*`), Bevel/Chamfer/Fillet
Edges, Subdivide, Split Edges, Flip Diagonal, Bridge, Cap, Hard/Smooth Normals, Mirror Mesh,
Mirror Faces, Live Mirror, Subdivision from Polygon; and, in a component mode, Delete, Cut,
Duplicate and moving components.

Does not ask (the triangles stay exact or are not touched): object moves/rotations/scales and
object mirrors, exact Booleans, CAD Detail, materials, UVs and seams, selection and mode changes,
collision hulls, Convert to Voxels, deleting the whole object.

## Code

- `src/editor_native_cad_guard.cpp`: `is_cad_mesh_edit`, `cad_mesh_edit_targets`,
  `ask_before_cad_mesh_edit`, `drop_cad_link`, `cad_link_dropped`.
- `PendingDestructiveAction::DropCadLink` reuses the existing Enter/Escape confirmation; the
  confirmation box now takes two lines and widens to fit.
- Hooks: `dispatch_action` (after the preview checks), `begin_gizmo_drag` and
  `transform_polygon_components` (component moves).

## Tests

`tests/test_editor_cad_edit_guard.cpp`: which actions ask; Escape keeps everything; Enter drops
the link and runs the edit as two undo steps; undo order; plain meshes, CAD Detail and exact
Booleans not asked; locked objects; confirmations off; component mode (Flip Normals, component
moves, whole-object delete).
