# Mesh authoring follow-up

Saved seams/pins are implemented by the later follow-up in
[Saved UV seams and pins](uv-seams-pins-2026-10-12.md); the session-only limitations
below describe the earlier implementation.

This builds on #112 with mesh Booleans, independent collision-hull authoring,
additional topology operations and a native UV workspace. These are working
editor paths, with bounded geometry APIs and atomic history commands. They do
not cover every feature of a mature modeling application.

## Mesh Booleans

Select exactly two owned polygon objects. The active object is A. Tools > Mesh
Booleans offers Union, A minus B, B minus A and Intersection. The preview draws
the proposed mesh without changing either input. In the preview, U/D/X/I choose
the operation, Tab swaps A/B, and R switches between a new object (default,
retain both inputs) and replacement of A (retain B). Enter applies one history
command; Escape closes the preview. Changing an operand invalidates the preview;
Enter refreshes it and requires a second press after inspection.

The backend is the vendored Manifold 3.5.1 library. Mesh positions are converted
to A's frame with the objects' world poses and nonuniform metrics. Both material
palettes, image/texture/sampler tables and material bindings are retained. Per-face
provenance maps the output faces to their source material. Vertex colors, both UV
channels and normal properties are interpolated through the solid operation;
tangents are refreshed. There is no voxel conversion.

Open, nonmanifold, degenerate, inconsistently wound or inward-wound inputs are
refused, as are empty or oversized outputs. These topology checks are not a
certificate that arbitrary input solids have no self-intersections. The software
preview is bounded and uses wire outlines, not production texture/depth rendering.

`DVE_ENABLE_MESH_AUTHORING` controls Manifold-backed editor operations and defaults
to ON. Builds without it retain explicit unavailability messages for Booleans,
convex hull construction and convex bevel; UV and elementary topology tools do
not require Manifold. Print export retains its separate build option.

## Collision hulls

Tools > Collision Hull > Fit Convex Collision Hull constructs a convex hull of
the mesh's geometric points. The final hull is limited to 256 control points.
Edit Collision Hull Points displays cyan collision geometry and point handles.
The visible polygon mesh is not replaced or deformed.

Click points; Shift adds to the selection; A selects all. X/Y/Z choose a local
axis. Arrows nudge by 0.1 local units. G/R/S select translation, rotation (degrees)
or uniform scale; type a value and press Enter to apply, or Escape to cancel.
With no transform selected, typing sets the selected point's axis coordinate.
N adds a control point near the selected point; Delete removes selected points.
B replaces the hull with its enclosing eight-corner bounds (an explicit coarse,
conservative simplification). F frames the hull. Escape closes the editor when
no numeric preview is pending. Flat, nonfinite and otherwise invalid hulls are
refused before metadata changes.

Points are stored in a versioned `dve.collision_hull` component. Existing scene,
prefab, clone and runtime component serialization retains them. Play and scene
export bake the polygon's geometry metric into the points, matching baked DMESH
vertices. Runtime component add/change/enable/remove rebuilds the body's collider
between ticks; invalid updates preserve the old component and body. Removing or
disabling the component restores the normal polygon proxy.

Jolt uses a convex point-cloud shape rather than the bounds boxes when a hull is
present. The reference solver retains approximate bounds-box queries. The current
Box3D adapter refuses authored hulls explicitly. Dynamic mass and inertia remain
bounds-based estimates; this does not claim exact solid mass properties. Hull
replacement during a simulation tick is refused, and live joint preservation is
not implemented. Keep collider changes in authoring or between ticks before
creating constraints. The pinned Jolt 5.6.0 headers compile-check the new shape
construction path; a live Jolt acceptance run remains separate.

## Topology tools

Enter a polygon component mode from #112, then select the appropriate components.

- Subdivide Selected Faces splits the selected faces and also splits neighboring
  triangles that share affected edges, avoiding T junctions across normal/UV seams.
- Split Selected Edges inserts midpoints and triangulates affected neighbors.
- Flip Triangle Diagonal works on a convex coplanar quad with compatible material,
  UV and normal attributes. It refuses deliberate seams rather than losing them.
- Bridge Boundary Loops connects two complete open loops with equal vertex counts.
- Cap Boundary Loop ear-clips one simple planar contour. Nested holes and
  nonplanar caps are refused.
- Bevel Edges (convex solid) opens a numeric preview for selected crease edges.
  The value is a mesh-local plane offset. Manifold clips the convex solid and
  generates real chamfer faces, with normals, planar UVs and a retained/default
  material slot. Concave solids and coplanar diagonals are refused. Enter applies
  one undo command, Escape cancels.
- Hard Face Normals and Smooth Selected Normals edit corner attributes independently
  of unselected faces and preserve UV and material data.
- Topology Diagnostics reports boundaries, nonmanifold edges, inconsistent winding
  and degenerate triangles, and highlights problematic edges in red.

The cooked representation still uses triangle faces. General edge bevel on
concave meshes, quad-aware loop cuts and true n-gon dissolve remain separate work;
Flip Diagonal is named accordingly and does not pretend to dissolve an edge.

## UV workspace

Tools > UV Workspace opens a 2D pane alongside the 3D view. The mode/tool controls
are clickable. 1/2/3/4 select vertices, edges, faces or islands; Q returns to
selection. Click or drag a box; Shift adds, Ctrl toggles, Ctrl+Shift intersects.
A or Ctrl+A selects all. UV selection highlights associated 3D faces; a 3D face
selection can be brought into the UV pane by clicking it. Tab switches UV0/UV1.

G/R/S select move, rotate or scale. Drag or type a number, constrain with X/Y,
Enter to apply or Escape to restore the mesh. Ctrl during a drag snaps translation
in 0.1 UV units or rotation in 15-degree steps. Middle drag pans; the wheel and
+/- zoom; F frames the UV selection. T switches checker/first-material base-color
texture display. H/V mirror around the UV pivot. D detaches the selected UV
patch with a visible +0.05 U offset; J stitches selected coincident geometric
points to their average UV. Neither operation welds the visible geometry.

U unwraps with least squares conformal maps, using sparse, matrix-free normal
systems and a checked conjugate-gradient solve. P toggles pins at the selected
corners' current UV coordinates; pins are independent per channel and remain
session-local. Disk charts keep their existing connectivity. Closed charts can
fall back to triangle charts; stitch/pack afterwards. Degenerate triangles,
nonconvergent solves, collapsed or flipped charts are refused. A chart solve is
limited to 20,000 vertices, and an unwrap/pack action to 20,000 selected faces.
These checks do not certify the absence of global chart overlap.

K packs selected islands into a deterministic grid in the unit tile, preserving
aspect ratios with an explicit 0.02 UV-unit gutter. If the selected island count
leaves no room for that gutter, the operation refuses rather than collapsing
islands. Packing is a simple layout, not an optimal atlas or texel-density solver.
Edits detach shared corner attributes as needed, so unselected faces and the other
UV channel stay unchanged. UV coordinates retain scene save/reload/export behavior.

Numeric and mouse UV previews block authored-content commands and manual save;
autosave pauses until commit/cancel. Focus loss restores the starting mesh.
UV pin flags, arbitrary seam flags, saved UV workspace state, UV-layout export,
UDIM workflows, overlap/stretch heat maps and texel-density tools remain future
work. This is a usable UV editing workspace, not a claim that those features are
finished.

## Validation

`dve_editor_mesh_authoring_tests` exercises Boolean volume/material/attribute and
refusal cases, seam-safe subdivision, diagonal flipping, capping, bridging,
convex edge bevel, UV corner isolation/unwrap/pack/pins/channel separation, hull
construction/validation/codec/runtime refusal, scene and export metric retention,
native Boolean/hull/UV workflows, history restoration and software-canvas drawing.
Existing polygon, voxel selection/Join/scale, shortcuts, autosave and command
availability suites provide regression coverage. Windowed artist validation,
large-asset responsiveness and live production-physics checks remain separate.

References: vendored `third_party/manifold/include/manifold/{manifold,mesh}.h`;
[Jolt 5.6.0 ConvexHullShape](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Collision/Shape/ConvexHullShape.h);
[Levy et al., Least Squares Conformal Maps, SIGGRAPH 2002](https://brunolevy.github.io/papers/LSCM_SIGGRAPH_2002.pdf).
