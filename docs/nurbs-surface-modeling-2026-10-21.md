# NURBS surface-modeling batch

All native actions retain their inputs and create an independent result in one
undo step. No live construction dependency or arbitrary boundary topology is
introduced. The original v1/v2 source codec remains unchanged.

| Tool | Implemented contract | Remaining scope |
| --- | --- | --- |
| Smooth loft | Compatible ordered snapshots; natural cubic interpolation in homogeneous coordinates; passes through each section at V=0,1,...; positive weights required | Alternative parameterizations, fairness optimization, automatic direction/seam/degree preparation and live links |
| Fit / rebuild | Fixed endpoints, polynomial least squares, degree up to three, 2..64 controls; reusable point-fitting API; curve rebuild checks 257 fit and 513 validation samples | Whole-curve error certificates, rational fitting, surface rebuild, knot removal, point-drawing UI |
| Adaptive tessellation | Surface polygon bake with a nonuniform tensor grid, sampled chord/normal refinement, knot-side normals, bounded work; original retained | Persistent adaptive display settings, asynchronous viewport updates, cross-object edge synchronization and certified bounds |
| Surface attachment | Other U-min to active U-max; identical V axes required; C0 positions or C1 homogeneous derivatives; creates a separate modified patch | Arbitrary edges, automatic degree/knot reconciliation, G1/G2/C2 tools, sewing and unified topology |
| Projection | Numerical constrained nearest-point query with four grid-selected seeds; sampled curve guide through projected points | Global closest-point certificates, persistent UV curves, directional projection, trim-ready curves-on-surface |
| Intersection | Intersection segment clipped to both affine planar parallelogram patches | Curved-surface intersections, tangencies, coincident regions and topological intersection graphs |
| Rectangular trim | Exact parameter-rectangle crop through source splitting; retained source and continuous parameter-based UV ranges | Arbitrary trim loops, holes, nesting, untrim history and trim-aware tessellation |
| Fillet | Exact rational circular arc tangent to a three-control degree-one corner; arc is a separate result | Automatic leg trimming/joining, general curve fillets, rolling-ball surface fillets and blends |

## Native use

Append ordered curve snapshots, then run **Smooth Interpolating Loft from
Snapshots**. The existing ruled loft remains available. Section correspondence
and input directions remain explicit. Smooth interpolation can overshoot; if
the resulting homogeneous weights are nonpositive, the action refuses it.

**Rebuild Curve** uses the existing Construction Settings field **Sections /
rebuild CVs / projection samples** as output control count. The native maximum
sampled error is fixed at 0.01 in source-local units and is reported in status.
The API accepts an explicit tolerance and returns its measured maximum error.
Rebuilding rational conics produces an approximation, with weights one.

**Bake Adaptive Surface Polygon** uses chord tolerance 0.01 local units and
normal angle 0.15 radians. It creates a normal polygon with the original
appearance and placement. The NURBS source and its existing fixed display cache
are retained. Nonuniform object scale can magnify the world-space error.
Subdivision is capped at ten refinements, 250,000 evaluations and 65,536
cell-corner vertices. Exhaustion returns no partial mesh. Global grid lines
avoid T-junctions within a patch; distinct objects do not share edge sampling.
Sampled midpoint tests can miss localized features and are not certified bounds.

For paired tools select two native sources; **the active source is the target**:

* Attachment: active U-max is fixed; other U-min and optionally its adjacent
  control column are matched. V degree, knots and row count must match exactly.
* Projection: active source is a surface; other source is a curve. The sections
  field sets sample count. Each output vertex is projected numerically; straight
  connecting segments generally leave a curved target surface.
* Intersection: both sources must be affine planar patches. Curved, parallel,
  coplanar, disjoint and point-contact cases are refused.

The other source is transformed into active-source coordinates before paired
operations. The result uses the active placement/parent and other appearance,
so object transforms are respected without changing either original.

**Crop Central Surface by Parameter Margin** uses the shared parameter field as
a margin on each of the four sides; set a value strictly between 0 and 0.5,
such as 0.1. This extracts the middle 80% of each parameter interval. It changes
the actual patch domain instead of storing invisible regions or trim loops.

**Create Corner Fillet Arc by Leg Fraction** uses that same field as tangent
distance divided by the shorter incident leg, strictly between 0 and 1.
It accepts exactly three controls of a degree-one curve. The radius API accepts
an explicit radius instead. The arc is circular in source-local coordinates;
nonuniform object scaling produces an ellipse in world space. Original legs
remain and require subsequent trimming/degree preparation/joining.

## Numerical contracts

Smooth loft solves the natural-cubic tridiagonal system for each homogeneous
control coordinate. Bezier endpoints are H[j], H[j]+M[j]/3,
H[j+1]-M[j+1]/3, H[j+1]. First and second homogeneous derivatives match; division
by a positive smooth weight preserves Euclidean C2 continuity. Full-multiplicity
Bezier knots describe this constrained net without implying corners.

C1 attachment uses a first-span/degree ratio to continue the reference's
homogeneous U derivative. It refuses nonpositive extrapolated weights. Matching
edges does not create a watertight topological solid or synchronize tessellation.

Fitting uses modified Gram-Schmidt with reorthogonalization rather than normal
equations and refuses rank-deficient systems. Its deviation is measured at
samples; nothing in this batch certifies all unsampled parameters.

Projection uses constrained Gauss-Newton steps and bounded line search from the
four best points of a 9x9 grid. It reports a converged numerical candidate,
not proof of the global closest point. Singular systems and exhausted iterations
return failure. It is bounded to four seeds, 40 iterations and 16 line-search
steps per iteration. Guides are limited to 257 samples. Projection and adaptive
tessellation validate the immutable source and cache its common weight scale once
per query, avoiding a full control-net validation on every hot-loop evaluation.

## Validation

The dedicated modeling suite checks section interpolation and smooth tangents,
positive-weight refusal, polynomial recovery, rebuild errors and metadata,
rational C0/C1 edge positions/derivatives and independent common weight scales, rectangular crop invariance and codec,
circular fillet radius/tangents, interior/boundary projection, clipped planar
intersections and explicit unsupported cases, adaptive density/budget/asset
validation, native one-command creation/undo/redo/selection/material retention,
source guards, and scene save/reopen. Existing NURBS, native polygon and SubD
regressions are also selected. Tests are headless; OS-window/GPU artist acceptance
and external CAD interoperability remain outstanding.

This batch is based on PR #166 (which depends on #165 and #164). It does not
claim complete Maya/Softimage surface-authoring parity.
