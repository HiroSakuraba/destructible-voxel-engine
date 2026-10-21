# Shape-preserving NURBS degree elevation

Adds **NURBS > Elevate U Degree (Keep Shape)** and **Elevate V Degree (Keep
Shape)**. Curve U and surface U/V can be raised one degree per action, up to
seven. This prepares differently authored sources for the existing equal-degree
loft and curve-join constructors without fitting a new shape.

The implementation first decomposes the chosen axis into rational Bezier spans
using homogeneous knot insertion. Each span is elevated by the standard convex
combination of neighboring homogeneous controls. Endpoints, parameter intervals,
texture ranges, display settings and the other tensor-product axis are retained.
Large common weight scales are normalized locally during each blend to avoid
intermediate overflow.

This version deliberately retains full-multiplicity boundaries between Bezier
spans. It preserves the existing geometric positions and first derivatives
within floating-point arithmetic, including existing smooth joins, but can use
more controls and repeated knots than a minimal-knot elevation algorithm. It
does not smooth corners, reduce degree, remove knots, rebuild to a target span
count or certify an approximation-error interval. The existing display sampler
may duplicate more span-boundary vertices while retaining matching normals at
geometrically smooth seams.

Native edits regenerate the source and cache in one history command and preserve
materials, world placement and scale. Control indices change, so selection is
cleared; undo restores the matching original source, cache and selected controls.
Save/reopen retains the elevated source. Locked/hidden/prefab sources and invalid
axis, maximum-degree, control-budget or display-budget cases refuse before scene
mutation. Existing source/runtime serialization remains unchanged.

To prepare mixed-degree loft or join inputs, elevate the lower-degree scene
curves explicitly, clear the construction queue and append fresh snapshots.
Elevation is not automatically applied inside a constructor, and old snapshots
are not live-linked. Endpoint matching, ordering and conditioning requirements
of the chosen constructor still apply.

`dve_editor_nurbs_degree_tests` compares rational positions and analytic first
U/V derivatives for degrees 1..7, large common weight scales, nonuniform knots,
repeated elevation and both surface directions. It covers exact endpoint
controls, conics, metadata, codecs, budget/axis/degree refusal, explicit input
compatibility, native source/cache/selection undo and redo, materials/placement
and scene save/reopen. The PR records executed regression results; windowed/GPU
artist acceptance remains separate.
