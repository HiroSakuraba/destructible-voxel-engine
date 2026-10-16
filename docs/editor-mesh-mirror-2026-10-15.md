# Half-model mesh mirror and live mirror

Model half a polygon mesh or subdivision cage, see the other half while you work, then mirror it
with a welded centre seam. Selected faces can also be mirrored on their own.

All of these are in the **Polygon** menu, under Symmetry. They work on the selected mesh, or on the
mesh being edited in the polygon selection mode. The mirror plane is the mesh's **own** plane
through its local origin: Mirror X uses the local plane x = 0, and so on. So model the half on one
side of the object's origin.

## Commands

| Command | Does |
| --- | --- |
| **Live Mirror X / Y / Z** | Shows the mirrored half, tinted, drawn from the half you are editing every frame. The menu checks the active axis; choosing another axis switches, and choosing the same axis again turns it off. It is saved with the scene (a `dve.live_mirror` component) and is one undo step. It is a preview: Play, export and picking only see the real half until you apply it. |
| **Apply Live Mirror (weld seam)** | Bakes the live mirror with Mirror Mesh on its axis and turns the live mirror off, as one undo step. |
| **Mirror Mesh X / Y / Z (weld seam)** | Adds the mirrored copy of every triangle and welds the vertices that lie on the plane, so the two halves share the seam. Seam vertices are snapped onto the plane, and their normals are turned to lie in it so the join shades smoothly. Triangles lying on the plane (a cap on the open side) are removed, because they would end up inside. |
| **Mirror Selected Faces X / Y / Z** | Polygon selection mode, faces selected. Adds a mirrored copy of just those faces, welded where they touch the plane, and selects the copies. The rest of the mesh is untouched. |

The status bar reports how many seam vertices were welded and how many faces on the plane were
removed. Every command is one undo step.

## What carries over

- **Persistent element IDs:**
  - original vertices, edges, corners, triangles and faces keep their IDs;
  - the mirrored half gets new IDs, with the source vertices recorded as their ancestors;
  - faces removed from the plane retire.
- **Subdivision cages:**
  - control faces (quads) are kept and mirrored with reversed winding, so the copy pairs the same
    way;
  - creases are kept and copied onto the mirrored edges;
  - the cage still subdivides.
- **Saved UV seams and pins** are remapped through the same lineage. An edit that would drop one
  without a mapping is refused instead.
- **Materials:** submesh material and name runs are kept, and mirrored triangles use their
  source's material.

## Refusals

Each refusal says why:

- a mesh (or selection) that **crosses the plane**: "keep one half on one side (Slice can cut it)";
- everything lying on the plane: there is nothing to mirror;
- Mirror Selected Faces without selected faces;
- a locked, hidden or isolated-out mesh;
- an open preview or edit, Play, or a long job running.

The weld tolerance is 1/10,000 of the mesh's largest size.

## Code

- `include/dve/editor_mirror.hpp` / `src/editor_mirror.cpp`: `mirror_polygon_half` returns the
  welded mesh, the updated subdivision settings (control faces and creases), the lineage for IDs and
  UV records, and the new faces.
- Controller (`src/editor_native_polygon.cpp`): `mirror_mesh`, `toggle_live_mirror`,
  `apply_live_mirror` and `live_mirror_axis`.
  - The edit is one `CompoundCommand`: `ReplacePolygonCommand` with the identities from
    `advance_modeling_topology`, plus the UV record and the removal of the live-mirror component.
  - The live preview is added to `polygon_draw_items`.
- `dve.live_mirror` is registered in the component registry with its `axis` property.
- Menu items `mirror.live_*`, `mirror.apply_live`, `mirror.mesh_*` and `mirror.faces_*`, with
  enabled states and a check for the live axis.

## Tests

`tests/test_editor_mesh_mirror.cpp` (`dve_editor_mesh_mirror_tests`):

- **Mirror core:**
  - the half cube becomes a closed, consistently wound 2×1×1 box with outward faces;
  - the two on-plane triangles are removed;
  - the 4 seam corners are shared;
  - a mesh crossing the plane is refused;
  - selected faces are copied with shared seam corners.
- **Command:**
  - original vertex IDs are kept and the mirrored half gets new ones;
  - the status reports what happened;
  - one undo restores the half and its IDs;
  - Mirror Selected Faces selects the copies and undoes;
  - a locked mesh is refused.
- **Cage:** 5 + 5 control faces, the crease is copied, the cage subdivides and is closed.
- **Live Mirror:**
  - it draws the other half while the mesh is unchanged;
  - switching axis, the menu check, and toggling off all work;
  - Apply welds the mesh and turns the live mirror off;
  - undo brings both back.

Updated: the "mirror selected faces" description avoids the word "copy", so Edit > Copy stays first
in the Command Center search for "copy" (`dve_editor_cage_edge_selection_tests`).

## What's not done

- **Symmetric editing of an already-mirrored mesh**, where moving a vertex moves its twin. Live
  Mirror covers the half-model workflow.
- **A mirror plane through a chosen point**, rather than the mesh's own origin. Move the object's
  origin (or the half) instead.
- **Live mirror in Play and export:** it is a preview; apply it to make the geometry real.
- **Bisect:** the mesh is never cut at the plane, so a mesh that crosses it is refused.
