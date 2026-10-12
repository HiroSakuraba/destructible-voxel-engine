# Polygon editor draft and Muse handoff

This is a draft, not a completed polygon editor release. The execution environment
stopped responding during native wiring; the original tree was published untested.
See "Status after the first CI run" below for what has since been built, tested and fixed.

## Status after the first CI run (follow-up fixes)

The tree has since been built and tested (Linux Release locally with the full ctest suite,
then CI). Fixes made on top of the original draft:

- Slice caps: ear clipping now treats a loop vertex lying on the new diagonal as blocking,
  so collinear contour points (e.g. a cube cut through its face diagonals) no longer leave a
  zero-area remainder and the back half of a capped slice is produced.
- The viewport preview clips triangles against the near plane and keeps triangles that are
  partly off screen; a large plane near the camera no longer disappears. Faces are shaded by
  their angle to the view.
- Honest commands (#101/#103): the component tools stay disabled with a "Not in this editor
  yet: polygon mesh editing ..." reason. Create > Polygon Cube and Polygon > Create Mesh Plane
  are real commands (one undo step, disabled with a reason during Play, new objects stay
  visible while isolated). Polygon > Import Polygon Model refuses with the Assets-panel reason
  when nothing is selected and imports owned meshes (DMESH/OBJ/glTF/GLB) as a cancellable job.
- Scenes are written as format 7 only when they contain owned polygon objects; other scenes
  stay format 6 (with the #109 axis scale), so older editors can still open them.
- Validate no longer reports owned polygon objects as empty voxel objects; Flood Fill refuses
  polygon objects explicitly.

## Code in this draft

- Owned CookedPolygonAsset data in EditorObject, deep cloning and geometry ID remapping.
- Scene format 7 writes edited DMESH assets transactionally; formats 1–6 remain readable.
- Native polygon cube/plane creation and cancellable OBJ/glTF/GLB/DMESH import.
- Meter-space triangle picking, polygon bounds/rectangle selection and bounded software
  triangle previews, with live runtime poses.
- Existing object selection, grouping, copying and transform workflows retain meshes.
  Positive voxelScale is reused as the local geometry metric for mesh positions;
  voxelSizeMeters is irrelevant to polygon geometry. Ctrl voxel resampling is refused
  for polygon selections.
- Edited meshes, rather than their original source files, are exported. Axis metrics
  are baked into vertices/normals/tangents for the runtime. Owned polygons enter Play
  through GameWorld's existing polygon APIs.
- Fill and brush scatter recognize polygon surfaces. Mesh copies retain their palette,
  texture tables and geometry; scale variation changes their geometry metric.
- Core vertex/edge/triangle-face selection, component transforms, deletion/duplication,
  region extrusion, convex coplanar inset/face bevel, weld, normal editing, planar UV
  projection, material assignment, Slice, Join, Extract and Separate APIs.
- ReplacePolygonCommand preserves complete mesh data and prefab geometry overrides;
  repeated brush commands can merge by stroke ID.
- New regression test target: dve_editor_polygon_tests. These tests are written,
  not yet run on this exact tree.

## Follow-up native component wiring

The follow-up in `docs/native-polygon-editing-2026-10-12.md` implements component
selection, highlights, gizmos, history snapshots, clipboard, tool previews,
materials, Join/Extract/Separate and world-axis Slice. The numbered list below is
retained as the original design checklist; mouse-draggable arbitrary Slice planes
and brush strokes are still outstanding. Consult the follow-up for actual scope
and validation rather than treating this old checklist as current missing status.

## Original native component wiring checklist

Component controls must have working native handlers and validation before enablement;
the follow-up supplies these handlers for the supported operations.

1. Extend SelectionDomain with Polygons and give NativeEditorController a
   PolygonSelection. Extend EditorSelectionSnapshot with polygon domain, object,
   mode and component IDs. Capture and restore these alongside topology Undo/Redo.
   Prune removed components after operations or object deletion.
2. Enable polygon.vertex_mode/edge_mode/face_mode/object_mode/component_toggle in
   Edit mode for an unlocked owned polygon. Activate ShortcutContext::PolygonEditor
   only while that domain owns the viewport; preserve camera gestures and text input.
3. Route click/drag release to pick_polygon_component/polygon_components_in_rect.
   Support replace/add/subtract/toggle/intersect, select all/invert/clear and connected
   islands. Through selection bypasses self-occlusion; visible selection must also
   filter occlusion by other scene objects through pick_editor_document_all.
   Box selection currently uses component centers; add crossing-edge behavior if desired.
4. Draw selected vertices/edges/triangle faces and a component count. Put the
   Move/Rotate/Scale pivot at the selected geometric vertices, not the object bounds.
   Snapshot the original mesh at drag start. Preview using world_to_voxel and
   voxel_to_world, with pivot rotation performed in world space. Restore the starting
   mesh on Escape/focus loss; on release execute one ReplacePolygonCommand.
5. Bind edit.cut/copy/paste/delete/duplicate to components in this domain.
   extract_polygon_faces supplies a component clipboard. Keep full-object clipboard
   behavior in the object domain. Duplicate overlapping faces cannot be distinguished
   by positional seam identity: use an explicit offset, or add separate topology IDs
   before supporting zero-offset duplicate-and-move.
6. Wire Extrude/Inset/face Bevel/Weld/UV projection parameter fields, normals and
   active-material assignment to edit_polygon. Validate values before mutation.
   An empty Delete result should remove the object through RemoveObjectsCommand;
   it must not install an invalid empty DMESH. Selected material indices refer to
   the mesh palette; import the active editor material into that palette first.
7. Route polygon Join/Extract/Separate through CompoundCommand and record both
   selections. Join meshes in the primary frame; preserve or explicitly reparent
   children before removing operands. Do not silently delete unrelated descendants.
8. Bind polygon Slice to a world plane and Front/Back/Separate outputs. Convert
   the point with world_to_voxel(pose, scale, point), and the plane normal with
   normalize(scale * inverse_rotate(rotation, worldNormal)). Execute replacement and
   optional new part together. Open meshes can opt out of caps.
9. Route Paint/Remove brush hits using EditorPickResult::polygonFace. The draft
   currently refuses voxel brushes on polygons to avoid creating hidden voxel data.
   Merge painting history by stroke ID and preserve component selection transitions.

Example backend use (independent of any native event handler):

~~~cpp
auto* object = workspace.document().find_object(selection.object);
if (!object || !object->polygon || object->flags.locked) return false;
PolygonEditSettings settings;
settings.amount = 0.25F;
auto edited = edit_polygon(*object->polygon, selection, PolygonEdit::Extrude, settings);
if (!edited) { show_error(edited.error); return false; }
auto command = std::make_unique<ReplacePolygonCommand>(
    object->id, *object->polygon, std::move(edited.mesh), "Extrude polygon faces");
const auto result = workspace.commands().execute(workspace.document(), std::move(command));
if (result.success) selection = edited.selection;
// The event handler must also record before/after polygon selection snapshots.
~~~

## Scope that needs additional algorithms

Face Bevel is a tapered region extrusion; it is not a general edge chamfer.
Implement edge bevel with proper adjacent-face reconstruction, stable material/UV
transfer and manifold checks. Inset/face bevel currently refuse nonconvex, disconnected
or noncoplanar patches. Slice caps refuse open, branched or nested contours and limit
contours to 4096 points. Add hole-aware triangulation before relaxing these checks.

UV tools are planar projection, not a full UV island editor or automatic unwrap.
Mesh Boolean CSG, mesh sculpt brushes and edge-loop topology tools are not implemented
by this draft. Use a robust mesh backend for CSG and test attribute/material transfer;
do not redirect those operations to voxel resampling.

The software preview is bounded and painter-sorted, with flat material colors.
It is not a texture-accurate or per-pixel depth renderer; near-plane clipping and
intersecting surfaces need the production/reference renderer path. Scene object
marquee triangle testing uses projected triangle bounds and needs exact intersection
tests before treating empty-corner selection criteria as complete.

## Validation in Muse

Configure/build the native editor and CPU player in Muse's environment. Build and
run dve_editor_polygon_tests, then the existing editor, scene composition/export,
continuous scale, voxel selection/Slice, scatter/brush/settings, polygon geometry/
rendering, game world/save and player suites. Check SOURCE_MANIFEST.sha256 after
every code change with scripts/update_source_manifest.sh.

Test a textured multi-material mesh with split normals/UV seams. Select and move
one vertex, edge and triangle; inspect for seams opening. Exercise X/Y/Z and uniform
scaling, rotate a component around its pivot, and repeatedly Undo/Redo. Verify
Escape/focus-loss cancellation and locked/hidden/isolated objects. Cut/paste and
duplicate components, Join and Separate objects, and Slice through edges, vertices
and holes. Reopen the scene and export the edited DMESH after removing the original
source file. Test prefab template updates and explicit geometry overrides.

Scatter voxel and polygon sources onto polygon and voxel targets, with pinned
sources, several surfaces, seed reproduction, yaw/scale variation, brush erase and
Undo/Redo. Verify true triangle normals and footprints. Play/Simulate the edited
meshes with reference and production physics. Ask artists to validate the complete
workflows before marking partial interaction criteria complete.
