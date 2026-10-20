# NURBS ruled loft and sampled one-rail sweep

This batch extends the native sources and control editing from
[nurbs-controls-refinement-2026-10-19.md](nurbs-controls-refinement-2026-10-19.md).

The next source operations are documented in
[nurbs-split-isoparms-2026-10-20.md](nurbs-split-isoparms-2026-10-20.md).

## Artist workflow

1. Select a native NURBS curve and choose **NURBS > Append Selected Curve Snapshot**.
   Append each curve separately in the intended order. The input list displays
   the count and the last three entries. Clear the list to change earlier entries.
2. For a loft, append two or more sections in loft order. **Reverse Last Input
   Direction** changes the last snapshot's parameter direction. **Shift Last
   Input Closed Seam One Span** cycles the seam of a closed, uniform,
   piecewise-Bezier curve, such as the rational circle primitive. It refuses
   arbitrary nonuniform or open curves. Both operations retain the scene curve.
3. Choose **Ruled Loft through Ordered Snapshots**. Sections must have the same
   degree. Knot domains are normalized and refined to a shared knot union.
   V is degree one: the result passes through each section, with straight ruled
   segments between sections. It does not fit a smooth interpolating loft.
4. For a sweep, clear the list, then append the profile followed by the rail.
   The profile's control points must lie in the plane through its object origin
   perpendicular to the rail's initial tangent (relative tolerance 1e-6).
   For example, use the XZ circle with a rail initially pointing along Y.
   The profile's offset vectors, orientation and nonuniform scale are retained;
   its origin is placed on the rail's start. The rail supplies placement.
5. In **Construction Inputs and Sweep Settings**, click the numeric fields for
   section count, total twist in degrees, and end scale. Enter applies; Escape
   cancels. Choose **Sampled Sweep (Profile then Rail)** to create a result.
   To change parameters, return to settings and build a new result.

The queue holds independent snapshots with object transforms applied. Editing,
removing or reselecting an input afterwards does not change a queued snapshot.
Append again after clearing to use a changed source. Construction settings and
input snapshots are transient controller state, not scene data or live links.
Results are ordinary editable NURBS sources: controls, weights, knot insertion,
undo/redo, scene persistence, polygon baking and Play/export follow the existing
native NURBS path. Creating a surface is one undoable command; input curves stay
in the scene. Settings and queue edits do not enter scene history.

## Geometry contracts and limits

- Loft accepts 2..64 ordered equal-degree profiles. It explicitly preserves their
  supplied directions and seams; it does not infer correspondence or reorder
  scene selection. Knot refinement is homogeneous. Each profile's weights are
  normalized by its largest weight before lofting. This preserves its shape and
  removes arbitrary global weight scale from the blend between sections.
- Sweep uses uniformly spaced **rail parameters**, not equal arc lengths. It
  parallel-transports section offsets using the shortest rotation between
  successive sampled tangents, then applies linearly interpolated twist and
  scale. This avoids a Frenet frame's normal flip at an inflection.
- Sweep is an approximation: a degree-one V surface connects sampled sections.
  The rail is followed at stations, with no certified chord/angle bound between
  stations. Increase section count and inspect the result. Unsampled rail
  reversals or tight features are not detected or certified safe.
- Closed/coincident-end rails, stationary sampled tangents and nearly opposite
  successive tangents are refused. There is no closed-rail holonomy correction,
  cap generation, self-intersection prevention, trimming, degree elevation,
  periodic-axis support or smooth V fitting in this batch.
- Sweep accepts 2..64 sections, absolute twist up to 23040 degrees and end scale
  1e-6..1e6. Existing 4096-control, sample-grid, coordinate, weight-conditioning
  and tessellation-degeneracy limits still apply. Sources that cannot produce a
  valid display mesh fail creation without adding an object.
- Scene rigid transforms use float quaternions/positions. Unit axes are transformed
  with the existing transform math, while control coordinates accumulate in
  double relative to the first input object's origin. Output placement is
  recorded in the creation command, including redo.

## Validation

`dve_editor_nurbs_construction_tests` checks section interpolation after knot
union/domain/weight normalization, rational reversal derivatives, cyclic seam
alignment, source codec, straight-rail twist and taper, transport through a
cubic inflection, station orthogonality/radius, valid tessellation, rejected
inputs and limits, native ordered snapshots with nonuniform object scale,
parameter-field input/cancel, retained source curves and creation undo/redo.
The existing NURBS, polygon, SubD, selection, snapping, scale, exact-CAD and
scene-export regression suites are also run locally. Native controller coverage
is headless; OS-window rendering and artist evaluation remain manual checks.
