# Polygon selection and grouping follow-up

Saved seams/pins are implemented by the later follow-up in
[Saved UV seams and pins](uv-seams-pins-2026-10-12.md); the session-only limitations
below describe the earlier implementation.

This batch follows merged PR #113. It is a source-reviewed change. No builds,
tests, or CI runs were performed for this batch, at the user's request.
The publication commit includes `[skip ci]` for the repository's push and
pull-request workflows.

## Polygon selection

Polygon > Grow Component Selection adds one adjacent ring. Shrink Component
Selection removes selected components touching unselected neighbors. Shrinking
a completely selected mesh leaves it selected; open borders alone do not erode it.
Select Connected Components expands to the connected region in the active mode
and no longer forces vertex or edge selection into Face mode.

Geometric identity crosses split UV and normal copies. Faces connect through
shared edges, so triangles touching only at one corner are not one face region.
Vertices connect along geometric edges; edges connect at shared endpoints.
Incidence buckets avoid storing complete adjacency cliques around high-valence
vertices. The scene geometry and component mode remain unchanged.

Drag a polygon marquee left to right for window selection: every endpoint/corner
of an edge or triangle must be inside. Drag right to left for crossing selection:
overlapping edges and triangles are included, including a triangle that fully
covers the box. Vertex box selection works the same in either direction.
Shift adds, Ctrl toggles, and Ctrl+Shift intersects.

Existing self/scene occlusion checks still use the component representative point
unless X-Ray is enabled. Components crossing the camera near plane are omitted;
full clipped, partially occluded component selection remains future work.

## Grouping and hierarchy selection

Edit > Select Parent, Select Children, and Select Siblings operate on hierarchy
objects and return to Object selection. Children means immediate children;
siblings means objects sharing the active object's parent, including the active
object. Invisible and isolated-out relatives are omitted.

Group uses an unused name (Group, Group 2, etc.), preserves selected subtrees and
the existing common-parent behavior, and records the selection transition for
Undo/Redo. The pivot uses the selected roots' world positions.

Ungroup selects the released objects and surviving selected owners. It deletes
only empty containers without owned polygon, text, Gabor or voxel geometry,
components, anchors, asset references, prefab links or membership data. A polygon
object with children therefore keeps its own geometry after its children are
released. Group/Ungroup record selection history.

Reparent history captures both sides' parent, attachment record and world cache.
Undo restores attachments cleared by detaching to a root, including socket and
inheritance settings; Redo restores the resulting state. History restoration
validates the full parent plan before changing objects. A failed initial batch
restores every original hierarchy/attachment record.

## UV interaction fixes

Minus reaches numeric input when Move/Rotate/Scale is active; in selection mode
it remains zoom-out. Middle-button pan uses the starting view center so multiple
pointer events do not repeatedly add the full drag distance. Switching transform
tools cancels an existing preview before choosing the new operation.

## Suggested manual acceptance checks

These are a handoff checklist, not claims of executed validation.

- Select vertices or edges across a cube's attribute seams, then Grow/Shrink and
  Connected. Confirm mode retention and the documented one-ring behavior.
- Drag a narrow box over an edge or inside a large triangle in both directions;
  compare window/crossing and X-Ray behavior.
- Group two objects beneath a common parent, then Undo/Redo. Confirm hierarchy,
  world placement and selection. Create another group and confirm a unique name.
- Ungroup a polygon parent with children and a parent carrying components.
  Confirm the owners survive and the released objects are selected.
- Detach/ungroup an attached child to the scene root, Undo/Redo, and inspect its
  socket, inheritance flags and local attachment transform.
- In UV Move and Rotate, type a negative value. Middle-pan through several
  pointer updates and confirm the final displacement depends on drag distance.
- Start a UV preview, switch transform tools, and confirm the old preview is
  restored before starting a new operation.

Persistent UV seams/pins, UDIM, layout export, overlap/stretch diagnostics,
texel-density tools, concave bevel, loop cuts and n-gon dissolve remain outside
this batch.
