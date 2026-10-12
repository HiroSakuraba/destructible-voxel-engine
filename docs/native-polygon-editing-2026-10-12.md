# Native polygon editing

This change connects the polygon toolkit from #111 to the native editor. Owned
polygon meshes now have an editable component domain, rather than disabled mesh
commands. Faces are triangles; geometric vertex/edge identity crosses split UV
and normal seams.

## Artist workflows

- Create a polygon cube/plane or import an owned polygon model, select it, then
  Tools > Polygon > Vertex / Edge / Face Selection Mode. In the viewport,
  1/2/3 switch component modes and 4 returns to object mode. Tab toggles object /
  component editing while an unlocked polygon is selected with a selection or
  transform tool. Shift+Tab retains panel focus traversal. M also returns
  component editing to object mode. Camera gestures keep their existing bindings.
- Click or box-select components. Shift adds, Ctrl toggles, Ctrl+Shift intersects,
  and Alt subtracts where the navigation profile does not reserve that gesture.
  Select All, Invert, Clear and Select Connected Shell act on mesh components.
  X-Ray Selection bypasses self and scene occlusion. Vertices/edges and selected
  triangle faces are highlighted, with a component count in the toolbar.
- Move/Rotate/Scale act on selected geometric corners. The pivot is the selected
  corner centroid. Translation/rotation use the selected world/local transform
  space; scaling uses the object's local frame, including nonuniform object
  metrics. Type a number during a drag; Enter commits; Escape or focus loss
  restores the original mesh. Alt scales uniformly. Snapping uses existing
  editor preferences. Nudge and Frame Selection also honor component selection.
- Extrude (Ctrl+E; E retains Rotate), Inset, Face Bevel, Weld by Distance and Planar UV Projection open live
  previews. Type a value, use arrows to adjust, Enter to apply or Escape to
  cancel. Inset values are fractions; extrude/bevel heights and weld distances
  use mesh-local units. Face Bevel uses typed height and arrows for inset.
  X/Y/Z choose the planar UV projection axis. Invalid values or unsupported
  topology restore the original preview mesh and block Apply.
- Recalculate Normals and Flip Normals use the toolkit. Assign Active Material
  to Faces imports the editor's active material into the mesh palette before
  assigning it, retaining the other materials and texture tables.
- Delete operates on components, including deletion of touching triangles in
  vertex/edge modes. Deleting all faces removes the object rather than installing
  an invalid empty mesh. Duplicate offsets copied triangles by 0.1 local X so
  positional component identity can distinguish the copies.
- Copy/Cut require triangle faces. Paste creates a new owned mesh object in the
  copied world pose/metric, retaining material/texture data. The most recently
  filled object, voxel or polygon clipboard determines Paste.
- Extract Selected Faces creates a sibling and removes the faces from the source
  in one history step. Join Polygon Meshes combines selected objects in the
  active object's frame and transfers material/texture palettes. Separate
  Connected Shells retains the first shell and creates siblings in the same
  world pose/metric. Join refuses operands with children; reparent them first.
- Slice Polygon Mesh previews a world-axis plane through the mesh bounds center.
  X/Y/Z choose its world normal; type a signed offset in meters. F/B/S choose
  front, back or separate outputs; C toggles caps. Separate previews the back
  half in blue and applies both objects in one undo step. Open meshes can opt
  out of caps. Unsupported contours and empty outputs block Apply.

Component selection, domain, mode, object IDs and geometry restore together on
Undo/Redo. Join/Separate/Slice refuse objects with component attachments or prefab
links; they do not silently invent transfer rules for those structures. Ordinary
component replacements retain the existing prefab geometry-override behavior.
Locked, hidden and isolated-out targets cannot be edited. Unapplied previews block
scene-changing commands and manual saves; autosave pauses during polygon previews
and component drags. Focus loss restores previews before returning control.

## Validation

`dve_editor_native_polygon_tests` exercises native mode shortcuts, click/box
selection, scene occlusion and X-Ray, seam-aware world-space movement on a rotated
nonuniformly scaled object, local scaling/world rotation, typed gizmo previews,
Escape/focus-loss restoration, topology preview cancellation/apply, palette
assignment, clipboard, extraction, complete deletion, child-protection in Join,
Separate and Slice, and geometry/selection Undo/Redo.

The existing polygon toolkit/storage/export, voxel selection and honest command
suites remain relevant. The software canvas path compiles without a GPU. Desktop
artist acceptance still needs a windowed run with textured multi-material imports.

Local validation on 2026-10-12: the optimized Linux Release controller/editor
compiled successfully and all 12 targeted CTest suites passed: native polygon,
polygon toolkit/storage/export, editor, autosave, shortcuts, continuous scale,
voxel Slice interaction, voxel selection, voxel Join, scatter, and both honest
command suites. The native desktop executable was unavailable because this
environment lacks X11/SDL development packages; windowed artist acceptance and
platform CI remain separate checks. Source manifest and whitespace checks pass.

## Still separate work

This is the native wiring of existing toolkit operations, not every item in the
artist inventory. Outstanding algorithms/workflows include:

- General edge bevel, loop cut, dissolve, bridge, boundary capping as an
  independent tool, and nonconvex/noncoplanar region inset.
- Mesh Boolean operations and a nondestructive modifier stack.
- Independent collision-hull controls and geometry-node authoring.
- A full UV island editor/unwrap, explicit hard-edge and smoothing controls,
  normal visualization, and a topology diagnostic overlay.
- Mouse-draggable arbitrary Slice planes, chosen cap materials and hole-aware
  triangulation for nested contours. Slice currently uses the existing simple
  contour cap algorithm and its 4096-point limit.
- Crossing-edge/triangle box selection: this batch uses the existing component
  center selection policy. Visible selection uses point samples and the bounded
  painter-sorted preview; it is not per-pixel production depth rendering.
- Mesh sculpt/paint/remove brush strokes. Face materials are assigned through
  component selection; voxel brushes refuse polygon geometry with a useful route
  to the polygon tools.

Artist acceptance criteria should remain partial for these broader features.
