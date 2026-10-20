# Measure and exact CAD snapping (20 Oct 2026)

Exact CAD, first batch, part 3. Builds on the CAD edit guard.

## View > Measure

Click two points; the distance shows on the line between them and in the status bar with its
X, Y and Z parts:

`Distance 583.095 mm  (X 500.000 mm, Y 0.000 mm, Z 300.000 mm)  from circle centre Ø 120.000 mm to vertex; exact`

- Points snap like a held-V move: vertices, edge midpoints, face centres, voxel and bounds
  corners, the surface. The target under the cursor is marked before you click.
- A third click starts a new measurement. Esc, or View > Measure again, ends.
- Readouts are in millimetres when a CAD object is involved, metres otherwise (Ben's choice:
  CAD readouts in mm, the rest of the editor stays in metres).
- `exact` means both points come from the exact shape; otherwise the readout says a point is on
  the triangles (whose accuracy the inspector shows: `Exact CAD: mesh within ±…`).

## On CAD objects

- **Exact snap targets.** Once a CAD object's exact shape has been read, its snap targets are its
  true corners, edge midpoints and circle centres (holes, bosses), not its triangles. This applies
  to Measure, held-V moves and the snap placement modes. A circle centre is a new target kind,
  ranked just after vertices.
- **Surface clicks** are projected onto the exact surface (brepkernel `query`, about a second in
  the background) and name the face: `Hole Ø 12.000 mm`, `Cylinder Ø 6.000 mm`, `Plane`,
  `Sphere Ø …`, `Cone 30.00° (included 60.00°)`, `Torus R …, r …`, `Freeform surface (NURBS)`.
  Sizes include the object's scale; on a stretched object curved faces say `before stretching`.
- **Reading the shape.** brepkernel `describe` runs once per exact source (keyed by its content
  hash) on a background thread, one source at a time, for visible CAD objects. Nothing waits for
  it; until it is read the object snaps to its triangles as before.
- **Older brepkernel.** If the installed brepkernel has no `describe`/`query`, Measure says so
  ("update brepkernel for exact measuring, snapping and STEP export") and falls back to triangles.
  These commands come with the brepkernel branch `feature/dve-cad-inspect-export`.

## Code

- `describe_exact_cad`, `query_exact_cad`, `describe_exact_face` and the `ExactCad*` records in
  `editor_exact_cad.hpp`.
- `src/editor_native_cad_measure.cpp`: Measure, the background descriptions.
- `src/editor_native_snap_targets.cpp`: exact targets for CAD objects, `SnapTargetKind::CircleCentre`.
- Renderer: hover marker, points, line and distance label.
- `dve_native_editor_x11`: replayed startup input (`--drag`, `--key-down`) waits for exact shapes to
  be read first, so headless screenshots match what an artist sees.
- brepkernel: `describe` also lists every edge (type, ends, midpoint, length).

## Tests

`tests/test_editor_cad_measure.cpp`: describe/query parsing, face names, the older-brepkernel
message, background reading, exact circle centre and edge midpoint targets replacing triangle
ones, two-point distances in mm (CAD) and m (plain), a surface click projected and named, Esc.
With the real brepkernel: a 12 mm cylinder's circles, faces and closest points, and in the editor
the top circle centre picked by Measure.
