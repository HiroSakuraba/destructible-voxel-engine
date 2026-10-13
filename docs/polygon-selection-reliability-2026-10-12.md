# Polygon selection reliability

This is a source-reviewed follow-up to PR #114. No builds, tests or CI were run,
at the user's request. The publication commit includes `[skip ci]`.

## Marquee selection

Crossing boxes now clip the geometric primitive against the camera's near/far
depth planes before projecting it, then clip the projected segment or polygon to
the viewport and selection rectangle. Off-screen endpoints remain usable.
A large triangle covering the box can therefore be selected even when its
original corners are outside the viewport.

Projection follows the current perspective/orthographic camera, physical field
of view and lens shift. Camera-space clipping uses double intermediates.
Screen-space intersections use perspective-correct world interpolation for
visibility queries. Render projection and ordinary point-picking APIs are unchanged.

Left-to-right window selection requires the entire original component to be
within the depth range and selection rectangle. Right-to-left crossing selection
uses the visible clipped overlap. Vertex selection remains point containment.
Depth clipping remains active with X-Ray, which bypasses occlusion queries.

Non-X-Ray marquee selection checks the center, vertices and edge midpoints of
the clipped overlap. Queries include scene geometry and the active isolation
mask, starting at the near plane so invisible geometry behind it cannot occlude.
The controller no longer applies a second representative-point rejection to box
results. The ordinary click-picking path keeps its existing checks.

Visibility is a bounded sampling approximation. A tiny visible opening between
samples can still be missed; this is not a per-pixel visibility or GPU selection
buffer. Very large coordinates still inherit the engine's Float3 precision limits.

## Component-mode conversion

Switching between vertex, edge and face modes on the same active polygon converts
the existing selection across geometric identity, including split normal/UV copies,
instead of clearing it. Entering component mode from Object mode or selecting
another object still starts with an empty component selection.

- Faces convert to their corner vertices or their own edges.
- Edges convert to their endpoints or touching faces.
- Vertices convert to touching edges or faces.
- Staying in the same mode retains the selection.

Conversion is incident selection, so converting upward can expand the selected
region. Switching repeatedly between modes need not reproduce the initial region.
No geometry or undoable scene content changes during mode conversion.

## Preview menus

Parent, Children and Siblings are disabled during polygon previews, UV/hull
numeric or mouse previews, active gizmo transforms, long jobs and import options.
Disabled reasons explain that the preview must be applied or cancelled.
Existing dispatch guards remain in place; active gizmo hierarchy selection also
has an explicit guard. Menu state refreshes when UV/hull keyboard and pointer
interactions start or finish a preview.

## Manual acceptance handoff

The following checks have not been executed here:

- Drag a crossing box over a long edge with off-screen endpoints, and inside a
  triangle covering the viewport. Compare with window selection.
- Repeat with an edge/triangle crossing the near plane, in perspective and
  orthographic views, with physical lens shift enabled.
- Cover a face center with another object while leaving part inside the marquee
  visible. Confirm normal selection can find the visible part and X-Ray includes
  the overlap regardless of occlusion.
- Isolate the target and confirm excluded scene objects do not occlude selection.
- Switch selected faces to edges/vertices, and selected edges/vertices to incident
  faces. Confirm geometry is unchanged and the documented expansion is respected.
- Open Extrude/Inset/Bevel or a UV/hull preview, and inspect Parent/Children/Siblings.
  Cancel or commit, then confirm those menu items become available again.

Saved UV seams and pins are the next separate authoring batch. This change does
not add UDIM, layout export, texel-density tools, concave bevel or loop cuts.
