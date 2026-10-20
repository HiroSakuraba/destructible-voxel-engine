# Exact convex collision for CAD parts

Physics engines collide convex shapes. Until now a polygon object collided as
its bounding box, or as one authored convex hull. Either way a hole fills in:
a peg dropped onto a drilled plate rests on top of it.

An object can now carry a `dve.collision_convex_set` component ("Exact Convex
Collision"). It holds a list of convex prisms whose union is the part, so a
hole stays a hole. brepkernel builds the prisms from the object's exact CAD
source and certifies them.

## Where the pieces come from

- **Import Exact CAD** and **Exact Booleans** build them automatically for
  parts that are straight extrusions: every face is a flat cap across one
  axis or a wall parallel to it. Plates with holes, slots, pins, stepped
  parts and L shapes all qualify, along any axis.
- **Polygon > Build Exact Convex Collision (CAD)** (`mesh.exact_collision`)
  builds them for an object with an exact source, for example one imported
  before this existed. It runs in the background and replaces any existing
  set in one undo step.
- Parts that are not extrusions (a chamfered or countersunk hole, a sphere)
  are refused by brepkernel. The import or Boolean still succeeds, the status
  line says why, and the object keeps its hull or bounding box.

brepkernel's `convex-pieces` command does the work:

1. Find the extrusion axis.
2. Cut the part into slabs between its cap levels.
3. Turn each slab's outline into polygons on the material side: arcs around
   holes get a polygon drawn outside them, arcs around material get chords.
   B-spline and ellipse edges, which exact Booleans often produce, are
   handled the same way.
4. Triangulate, then merge the triangles into convex pieces.

The certificate checks convexity, overlaps and the boundary topology (outer
loops and holes) exactly. It then measures that the pieces never reach
outside the part and that the part is within the polygon error of the
pieces (`ExactCadSettings::collisionMaxError`, default 0.1 mm). Its summary
is stored on the component.

## At runtime

Precedence for a polygon body: an enabled convex set, then an enabled
authored hull (`dve.collision_hull`), then the bounding box.

- **Mass:** with a convex set, mass, center of mass and inertia come from the
  exact prisms (closed-surface tetrahedra in double precision), not from the
  bounds.
- **Voxel scale:** the component is baked by voxelScale for runtime scenes and
  Play, like hulls.
- **Jolt:** builds a static compound of convex hulls. Each hull keeps its
  points to 1 um, where Jolt's default hull tolerance would have moved faces
  by up to 1 mm, and has no convex radius.
- **Other backends:** the reference solver keeps using the bounding box for
  its approximate queries. Box3D rejects convex sets, as it rejects hulls.

## Fine fits need two Jolt settings

Jolt's game defaults add contacts 2 cm before bodies touch and allow 2 cm of
overlap. That is fine for 1 mm clearances. With 0.1 mm, a peg catches on the
rim of its hole: Jolt makes a contact with the rim edge before the peg is
over the opening, and the peg comes to rest there. Two new project settings
(Physics > Jolt) fix it:

| setting | default | for 0.1 mm fits |
|---|---|---|
| Speculative Contact Distance (`physics.contact_distance`) | 0.02 m | 0.0005 m |
| Penetration Slop (`physics.penetration_slop`) | 0.02 m | 0.0001 m |

Smaller values cost some robustness for fast-moving bodies; continuous
collision still applies.

Measured with Jolt 5.6 (`dve_collision_convex_set_tests`, a 40 mm peg dropped
onto a 50 mm plate with a 20 mm square hole):

| clearance | settings | convex set | one hull |
|---|---|---|---|
| 1 mm | defaults | falls through | rests on top |
| 0.1 mm | 0.5 mm / 0.1 mm | falls through | rests on top |
| 20 um | 0.5 mm / 0.1 mm | falls through | rests on top |
| 0.1 mm | defaults | catches on the rim | rests on top |

## Limits

- **Shapes:** only extrusions. Countersinks, chamfers, fillets across the axis
  and freeform parts get no pieces.
- **Edits:** pieces follow the exact source, not mesh edits made afterwards.
- **Precision:** pieces are stored as float (about 1e-9 m at these sizes),
  like hull points.
- **Exported games:** the two new Jolt settings reach Play and Simulate
  through the project settings. Exported games create their physics world
  with Jolt's defaults.

## Tests

`dve_collision_convex_set_tests` covers:
- the codec and its limits, and baking by voxel scale;
- prism mass properties against closed forms, including a product of
  inertia;
- the body descriptors;
- GameWorld precedence, rebuilds and refusals (reference solver);
- with `DVE_ENABLE_JOLT`, the drop tests above.

`dve_editor_exact_cad_tests` covers:
- pieces on import and on exact Boolean results, the "not an extrusion"
  note, and switching collision off (fake brepkernel);
- with brepkernel installed, a real drilled plate with 105 certified pieces
  and the rebuild command with one-step undo.
