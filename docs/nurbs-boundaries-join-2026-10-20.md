# Native NURBS boundary curves and ordered curve joining

Builds on [display quality](nurbs-display-quality-2026-10-20.md). These are
source-preserving curve tools, not surface stitching or solid operations.

## Extract four boundary curves

Select one visible unlocked native surface and choose **Extract Four Boundary
Curves (Keep Surface)**. Four independent editable rational curves are added in
one undo command, retaining the original surface. Their order forms a loop
around the untrimmed parameter rectangle:

| Order | Parameter edge | Curve direction |
| --- | --- | --- |
| 1 | V-min | Increasing U |
| 2 | U-max | Increasing V |
| 3 | V-max | Decreasing U |
| 4 | U-min | Decreasing V |

Each curve retains its own axis degree, knot domain, rational weights and display
settings. The command uses boundary control strips rather than sampled fitting.
Inputs can have different U/V degrees. These are four **parameter edges**:
coincident edges at a geometrically closed seam are kept, not deduplicated, and
no trim loops or unique topological edges are inferred.

All four display meshes are prepared before any result is published. If any
ribbon cannot be baked, none is added. Results preserve the surface's world
placement, nonuniform scale, material/texture resources, tags, groups and layer.
They are unparented, collision-disabled guide ribbons; gameplay components,
attachments and children are not duplicated. The first boundary is selected in
object mode. Undo/redo removes/restores all four together. Detach prefab
instances before editing.

## Join ordered curve snapshots

1. Clear construction inputs, select the first native curve and choose
   **Append Selected Curve Snapshot**. Append subsequent curves in order.
2. Each preceding curve's end must exactly equal the next curve's start in the
   captured coordinates. Use **Reverse Last Input Direction** where needed;
   position/orient source curves before taking snapshots. Split pieces and
   shared boundary endpoints are useful starting points. Visual proximity alone
   does not establish an exact shared endpoint, especially after different
   floating-point object transforms.
3. Choose **Join Ordered Curve Snapshots (Matching Ends)**. A new editable native
   curve is created, retaining the original scene curves and snapshots. Creation
   is one undo command. Failed joins add no object or scene-history command.

The existing snapshot workflow applies object rotation and nonuniform scale in
world coordinates relative to the first input's origin. The result uses that
origin and default NURBS curve materials/flags, like the other snapshot
constructors; it does not inherit a different material per segment or duplicate
gameplay components. Later edits to retained inputs do not update the snapshots
or the joined output. Clear/reappend to use changed inputs.

## Join geometry contract

- Accepts 2..64 clamped curves of equal degree, with exactly matching adjacent
  endpoint positions. It refuses gaps, incorrect directions and mixed degrees.
  There is no implicit endpoint snap, refitting, smoothing or degree elevation.
- Maps each original knot domain to one consecutive unit interval: curve 0
  occupies 0..1, curve 1 occupies 1..2, and so on. This changes parameter speed;
  it does not provide arc-length parameterization.
- Retains interior knots and shares each common endpoint control. The join knot
  has multiplicity equal to the degree, allowing a positional-continuous corner.
  Tangent continuity is inherited only when the original pieces already have it;
  the tool does not enforce a smooth join.
- Rescales each piece's homogeneous weights to share the endpoint weight. Weight
  ratios are accumulated logarithmically and normalized globally to avoid
  multiplying large arbitrary input scales. The rational curve shapes are
  preserved within floating-point arithmetic, without interval certification.
- Combined weights must satisfy the existing 1e-12 minimum/maximum conditioning
  ratio. Individually valid pieces can therefore refuse when combined. The
  4096-control and display vertex/sample budgets also apply. Maximum input display
  density is used, and crease-aware display is enabled.
- Output guide UVs start over at 0..1; arbitrary per-piece texture ranges are not
  preserved across the new unit-interval parameterization. Geometrically closed
  endpoints remain clamped, not periodic, with no extra closing segment or cap.

A four-edge loop whose U and V degrees differ cannot be joined directly in this
batch. Each boundary remains individually useful for other curve/surface tools.
This does not add surface attachment, patch stitching, trims, intersections,
fillets, solid sewing, tolerance-based welding or tangent fitting.

## Validation

`dve_editor_nurbs_boundaries_join_tests` checks boundary positions and loop
orientation; joins across degrees 1..7, rational weight scales and nonuniform
knot domains; position/analytic derivative reproduction under unit-interval
remapping; split-curve reconstruction; codecs; tiny-gap/direction/degree and
weight/control/count rejection; mixed-degree boundary extraction; native
four-object undo/redo and resources; refusal after a later boundary bake fails;
transformed snapshot joining; retained sources; and recovery after reversing an
incorrectly oriented snapshot. The PR records executed regression results.

Native tests are headless; OS-window/GPU artist validation remains separate.
