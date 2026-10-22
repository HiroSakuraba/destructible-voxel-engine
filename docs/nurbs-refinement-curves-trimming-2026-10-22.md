# NURBS network refinement, surface curves and trimming

This PR implements the user's requested three batches together, based on main
`1b5aad0bf2d880490cb2c3680889a9febea1d66a`. It preserves the existing network
placement/reuse work. It does not contain the separate CPU fog PR.

## Network refinement

`refine_nurbs_network_command(id, alongV, knot)` requests exact homogeneous knot
insertion. A missing knot requests one degree elevation. Existing generic source
edits still cannot arbitrarily replace a linked parameterization. The trusted
refinement operation prepares the complete connected component, propagates the
compatible degree/knot union along affected tangential seam axes, synchronizes
the display schedule, validates C0/C1 constraints, and publishes one history step.
The operation can originate on a driven patch. Reversed and cross-axis seam
correspondence use the existing normalized-domain mappings.

The core exact operations provide the shape-preservation mechanism. An independent
25x25 parameter sample check on every member rejects incompatible reconstruction
before publication; this check is numerical, not an interval-certified surface
bound. Degree, control, network, iteration and display budgets remain enforced.
Every affected patch must be visible, unlocked and owned. Native source Insert
U/V and Elevate U/V actions use this coordinated path and clear stale CV indices.
Reverse/swap and the legacy control-mode insert actions retain their guards.

## Persistent curves on surfaces

The `dve.nurbs` component's optional `surface_features` string stores versioned,
validated `NurbsSurfaceFeatures`. Each curve has a stable ID, a rational NURBS UV
curve, an optional projection-source object ID and a display sample count. UV is
normalized to the parent domain. Controls must remain inside [0,1]^2 with z=0.
The original surface source/codec is retained.

`project_nurbs_surface_curve_command` samples a source curve in the target's local
frame, accounting for both world transforms and axis scales. It stores the
projected UV guide on the surface instead of making a detached 3D polyline.
`evaluate_editor_nurbs_surface_curve` evaluates those UV coordinates on the current
surface and applies its current placement. Therefore source-CV, display, refinement
and placement edits cannot leave stale guide geometry. The native viewport draws
selected surfaces' guides as an overlay, outside the exported surface mesh.

The stored UV attachment is authoritative. Projection provenance is retained for
an explicit undoable refresh from the source curve; moving that source does not
silently move a committed trim. Refresh is numerical nearest-point projection,
not directional projection or a globally certified closest-point solution. A
missing projection source makes refresh fail atomically; the stored UV guide can
still be evaluated and edited.

`set_nurbs_surface_features_command` edits arbitrary UV controls/weights/knots.
`NativeEditorController::edit_nurbs_surface_uv_control` exposes transactional CV
editing to UI clients and maintains coincident closed-curve endpoints. Curves
used by trim loops update those loops and the display in the same transaction.
This PR does not add a full Maya-style 2D UV curve editor or on-surface CV gizmo.

## Trimming

`NurbsTrimRegion` holds simple closed normalized-UV polylines, with implicit
closure. Winding does not need manual orientation: odd parity retains outer
loops and removes nested holes. Alternatively, one or more seed points select
individual containment regions, including holes or exterior regions. Inversion
selects the complement. Touching/crossing loops, self intersections, degenerate
edges/areas, boundary seeds, invalid coordinates and empty display regions refuse.

Persistent rational UV curves can supply loops via `trim_nurbs_surface_curve_command`.
Their sampled UV polylines are the trim boundary; these are not analytic NURBS trim
edges or a B-rep sewing operation. Sampling is explicit (3..257 guide samples),
with at most 32 loops, 2048 total loop vertices and 32 region seeds.

The tessellator partitions UV into vertical slabs at trim events, determines the
retained intervals, clips convex trapezoids against display-grid rows, and
triangulates retained polygons. It evaluates surface positions and one-sided
surface derivatives at generated parameters. It does not discard whole triangles
by a centroid test. UV texture ranges are independent of trim coordinates.
Cancellation, finite/precision checks and the 65536-vertex per-patch budget apply.

Trim events seed the shared network grid before seam synchronization. Trimmed
boundary vertices use the same canonical anchors as their neighbours. A network
has at most 524288 actual display vertices. Interior holes can coexist with
linked seams. Touching or removing a linked patch boundary refuses; unlink it
first. This avoids creating unsupported partial-seam topology.

Untrim removes only the trim region. It retains the original source and UV curves,
restores its current untrimmed display policy and is undoable. All feature edits
stage/validate the complete affected component. Stale source, placement, metadata,
display or ownership checks prevent partial history publication. Material resources
are preserved. Feature strings and the derived trim mesh survive scene save/reopen.
Adaptive polygon baking retains trimming. Native crop/split/reverse/swap, underlying
boundary extraction, detached boundary matching and planar intersections refuse
trimmed inputs until untrimmed, rather than silently filling holes.
Existing fixed/adaptive background preparation uses the same trim-aware display
function.

## Native actions

- Project Other Curve as Persistent UV Guide.
- Refresh / Remove Latest Surface Curve.
- Trim with Latest Closed Surface Curve (adds a loop or nested hole).
- Untrim Surface (keeps UV curves).
- Invert Retained Trim Regions.
- Keep Hovered Trim Region (a retained visible region can be picked in the viewport).

The command API accepts arbitrary normalized-UV region seeds, including regions
currently absent from the display; the viewport action needs a visible surface
hit. The older detached sample-guide projection action remains available.

## Validation

`dve_nurbs_trimming_tests` checks retained area for nested holes, concave and sloped
loops, inversion and seeded regions, plus invalid topology, texture ranges and
cancellation. `dve_editor_nurbs_surface_batch_tests` exercises directed-network
insertion/elevation, rational reversed/cross-axis seams, independent shape checks,
shared trim schedules, locked-member atomic refusal, guides after surface edits,
UV edits, background preparation, native actions, history and save/reopen.

Local validation passed all 19 selected headless NURBS/editor/chrome-fit suites.
The two new suites also passed AddressSanitizer + UndefinedBehaviorSanitizer with
six changed geometry/editor translation units instrumented; dependency archives
were otherwise uninstrumented and leak detection was disabled.

The existing NURBS suites remain regression coverage for source codecs, attachments,
network edits, reuse, worker cancellation and native interaction. Validation is
headless; no windowed artist acceptance or full Maya/Softimage parity is claimed.
General curved-surface intersections, partial-seam trimming, analytic trim edges,
surface fillets/offsets and a full UV curve editor remain later work.
