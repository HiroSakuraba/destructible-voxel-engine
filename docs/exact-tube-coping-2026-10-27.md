# Exact tubes, coping and plane (miter) cuts

Fabricating a tube joint needs a cut face that lies **on** the true surface or plane
through the inner and the outer wall, not on a tessellated approximation of it. This adds
three Polygon-menu commands built on the existing exact CAD Booleans
([exact-cad-boolean-2026-10-13.md](exact-cad-boolean-2026-10-13.md)). The motivating job is a laser-cut
hollow rectangular tube on a 47.5 degree plane, where the cut face must be one plane through
both walls.

Everything is one certified brepkernel operation or a refusal with brepkernel's reason:
nothing in the scene changes on a refusal, and no result is ever produced without the
certificate. brepkernel itself is not modified.

## Commands

All three are in the Polygon menu and the Command Center, disabled with the reason when they
cannot run (Play or Simulate active, a long job running, nothing or the wrong thing selected,
locked or isolated objects, no exact source, no tube record, a brepkernel that is missing).
Each runs brepkernel as a background job (Esc cancels) and makes **one undo step**: a new
object, so the operands stay as they were. Undo removes it, redo restores it with its exact
source and tube record.

- **Create Exact Tube (CAD)...** (`polygon.create_exact_tube`). Type
  `round 50.8 3 600 y`, `square 40 2 500 x` or `rect 80x40 3 600 z`: outer size(s), wall,
  length (millimetres; the suffixes `mm`, `cm`, `m`, `in` work on any number) and the axis
  letter x, y or z (default y). The tube is the outer solid minus a longer bore, as one exact
  Boolean, centred on its own origin. Checks at creation: one solid, watertight, volume within
  1e-6 of the analytic one. The wall must be at least 0.1 mm and less than half the smallest outer size.
- **Cope Tube A to B (Exact CAD)...** (`mesh.exact_cope`). Select two tubes; the active one is A,
  the other is B. A minus B's **solid outer body** (not its bore), so A's end is saddle-cut
  through both of its walls onto B's outside surface. Type the fit-up gap in mm (default 0): the
  cutter is B's outer body grown by the gap on every side.
- **Plane Cut / Miter Tube (Exact CAD)...** (`mesh.exact_miter`). Select one tube and type
  `47.5` (the angle from a square cut). Options: `high` or `low` (the end kept, default low), `u` or `v`
  (the transverse direction the plane leans toward, default u), `at 120` (where the plane
  crosses the axis, mm from the tube centre, default 0). A large cube slab on the removed side of
  the plane is subtracted, so the cut face is one exact plane through both walls.

The inspector shows `Round tube Ø50.8 x 3 mm wall, 600 mm long, along Y; 2 cuts` for a tube
while its exact link is present. Cuts are recorded (`cope to B, gap 0 mm`,
`plane cut 47.5 deg from square, tilt u, at 50 mm, keep low end`) and carried to the next result,
so a cope can be followed by a plane cut. The `dve.exact_tube` record cannot be edited by hand
(the editor says to make a new tube): it must describe the exact B-rep next to it.

## Equal diameters at 90 degrees

brepkernel refuses two cylinders of equal radius crossing at right angles (their surfaces touch
tangentially along the crossing: `OpenAssembly`). The cope handles that case in one documented way and no other:

1. The cope is tried at the requested gap. Any brepkernel refusal is reported as it is, except
2. when the pair is near-tangent (a width of A within 0.04 mm of a width of B plus twice the gap),
   one retry is made with **0.01 mm** extra clearance (`ExactCopeOptions::tangentClearanceMeters`).
   brepkernel accepts that, and the result says so: *"Equal outer sizes touch tangentially and
   brepkernel refuses that at zero gap, so this cope uses 0.01 mm of clearance: the mating tube is
   0.02 mm larger across than drawn. It is certified for that geometry, not for zero gap."* The
   same text is stored in the cut record. If the retry is refused too, both reasons are shown and
   nothing changes.

Equal squares at 90 degrees are certified at zero gap and need no retry. Pass an explicit gap
(for example 0.2 mm) and the first attempt is already clear of the tangency.

## Limits and honest caveats

- **No outer corner radius.** brepkernel's primitives are sharp boxes and cylinders, so a
  rectangular or square tube has square outer and inner corners. Rounded corners would need a
  brepkernel primitive or a fillet; they are not simulated.
- **Uniform scale for gaps and angles.** A fit-up gap needs a uniformly scaled B and a plane cut a
  uniformly scaled tube (an angle is not true on an unevenly stretched one); otherwise the command
  refuses and says so. A cope at gap 0 uses the exact placement of B in A, scale included.
- **The plane must cross the side walls.** A plane that would leave through the end face of the
  tube (very shallow, or near the end) is refused with the allowed position. Angles are limited to
  within 85 degrees of a square cut.
- **B must be a tube made here** (it has a tube record): its outer body is known exactly. A is any
  exact CAD object; without a record its result has none either.
- **A cut that passes all the way through splits A.** The result then has several pieces and a
  warning says so. A cut that removes nothing is refused, not silently accepted.
- **The mesh is tessellated, the result is not.** Volumes, plane distance and wall thickness are
  checked on the exact B-rep; the displayed mesh follows within the mesh accuracy
  (Polygon > CAD Detail). A round tube's mesh volume is a little under the exact one.
- **Collision.** Straight tubes get certified convex collision pieces if brepkernel's collision
  extra (`scipy`, `shapely`) is installed; cut tubes are not extrusions, so they keep one convex hull.
- **Windows.** The process runner has not been run on Windows (same as the other exact CAD commands).
- Typed values are millimetres; the editor does not yet have a tube-editing panel: change a tube by
  making a new one (the previous one stays until deleted).

## Tests

`dve_editor_exact_tube_tests`:

- **Always runs, no brepkernel:** parsing and validation of tube and cut specs, the tube record's
  save/load round trip and its removal from runtime export, the arguments sent to brepkernel
  (against a stand-in runner), the tangent retry and each refusal, the plane-cut placement algebra, the menu entries,
  their descriptions and disabled reasons, typed-input errors, a missing brepkernel, the hand-edit guard.
- **With brepkernel (about 5 minutes):** tube volume against the analytic volume, wall
  thickness from the exact faces, plane-cut faces within 1e-9 m of the plane (47.5 degrees on a
  rectangular tube, both ends, both lean directions, a scaled and a round tube),
  cope volumes against an independent numerical integral (round on round at 90, 60 and 30
  degrees, offset axes, round onto rectangular and back, a fit-up gap, pass-through), closed meshes, the
  equal-diameter cases, and the native commands end to end (create, cope, plane cut, undo/redo, refusals,
  scene save/load).
- Without brepkernel those parts print `SKIPPED` and the test exits 77 (ctest "Skipped").
  `DVE_REQUIRE_BREPKERNEL=1` makes a missing brepkernel a failure.

CI: the `Brepkernel` test group (cmake/DveTestCoverage.cmake) is built only where brepkernel
can run. No existing job has brepkernel, so the new `exact-tube.yml` workflow installs a pinned
brepkernel and runs the tests with `DVE_REQUIRE_TEST_GROUPS=Editor,Brepkernel`,
`DVE_REQUIRE_BREPKERNEL=1` and `DVE_REQUIRE_COMPLETE_TESTS=1`, so nothing can skip there.
