# Native NURBS: first editable slice

This adds scene-owned NURBS source data with a derived polygon display/runtime
bake. It is an initial untrimmed curve/surface workflow, not Maya/Softimage parity.

The next controls/refinement batch is documented in
[nurbs-controls-refinement-2026-10-19.md](nurbs-controls-refinement-2026-10-19.md).
The limits below describe this initial slice.

## Artist workflow

Use the new **NURBS** menu:

- **Create Editable Patch**: cubic 4×4 control net.
- **Create Rational Circle / Create Quarter Arc**: rational conic source curves,
  displayed as narrow two-sided guide ribbons.
- **Create Revolve Profile**: cubic vertical profile.
- **Create Revolved Surface**: example profile revolved around local Y.
- **Extrude Selected Curve (2 m Y)** / **Revolve Selected Curve (Y Axis)**:
  new surface, retaining the curve. These initial menu constructors have fixed
  distance/axis/angle; the core API accepts extrusion vectors and angles.
  Revolving a curve in the XZ plane may be singular and is refused; use the
  vertical profile for the sample revolution.

Creation opens a control panel. Click a control point, including hidden points
(the overlay explicitly uses X-ray selection), or use Previous/Next in the
panel/menu. Click X, Y, Z or Weight, type a number and press Enter. Escape
cancels the field. Source and derived mesh update as one undo command.
Coordinates are local, before object scale. Weights must be positive.
Finish Control Editing closes the overlay; Edit Control Points and Weights
reopens it.

Whole objects use existing transforms. Multi-CV selection and shared component
gizmos are follow-up work; this release uses numeric single-control editing.
CV indices remain stable because there are no topology-changing knot tools yet.

**Bake to New Polygon (Keep Source)** adds an independent polygon for existing
mesh tools. The native source stays editable. Triangle/mesh UV edits and
polygon-to-SubD conversion refuse native sources. As a source-authority safeguard,
ReplacePolygonCommand also refuses retained Exact CAD Source objects. Explicitly
remove that component to detach an imported CAD bake before triangle edits.

## Persistence and runtime

Native source is stored as a versioned dve.nurbs component beside its derived
mesh cache, through existing scene/component codecs. Save/reopen and cloning
retain source data. The inspector directs artists to NURBS tools instead of
showing the encoded control net. Generic source-property writes and disabling
the source are refused. Detach prefab instances before source editing; general
native-source prefab overrides are not implemented.

Play and runtime export strip editor-only source and use existing cooked meshes.
A shipped baked scene needs no NURBS editor or CAD subprocess. New NURBS objects
are anchored and collision-disabled: open patches/guide ribbons do not imply
closed volume. Author collision on a separately baked polygon if appropriate.

Validation rejects invalid/disabled source, conflicting SubD/CAD metadata, and
mesh positions/connectivity inconsistent with a fresh source bake. Native edits
preserve existing material/texture resources. Arbitrary cache UV editing is blocked.

## Kernel contract and limits

- Double-precision rational tensor-product evaluation and analytic first U/V
  derivatives. Full knot convention: count = controls + degree + 1.
- Clamped open axes, degree 1–7. Geometric closure uses matching endpoints;
  periodic forms and singular poles are not supported.
- Curves use a degree-zero V axis, one row and knots 0,1.
- Interior knots choose the right-hand span; the upper endpoint chooses the left.
- Limits: 4,096 controls, 1–64 samples per nonzero span, 65,536 source samples,
  1 MiB encoded input, position magnitudes at most 1e8, and a minimum/maximum
  positive weight ratio of at least 1e-12.
- Common weight scaling is normalized. Bad parameters, nonfinite derivatives
  and ill-conditioned denominators fail explicitly.
- Fixed per-span tessellation, analytic normals and normalized-parameter texture
  coordinates. No certified chord-deviation bound. Singular samples and
  float-collapsed triangles refuse the bake.
- General C0 interior crease-normal splitting is follow-up work; the conic
  constructors have geometrically smooth construction seams.
- No trims, knot insertion/removal, patch continuity, adaptive tessellation,
  solid operations, second derivatives or fitting yet.

## Validation

dve_editor_nurbs_tests covers rational conics/derivatives, affine reproduction
for degrees 1–7, non-uniform rational evaluation, extrusion/revolution, bounded
codecs, invalid and degenerate inputs, source/cache protection, commands,
undo/redo, cloning, save/load, runtime export/load, native pointer/text input,
cancel, retained-source baking and Play.

Native tests use the real controller and software-canvas contract without an
OS window. Windowed GPU/artist validation remains separate. The PR records
the executed results and build configuration.

