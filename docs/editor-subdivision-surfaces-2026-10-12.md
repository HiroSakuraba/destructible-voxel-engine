# Subdivision surfaces: third geometry workflow

Follow-up to PR #118. Source review only: no builds, automated tests or CI were run.
The commit includes [skip ci]. Manual acceptance steps below have not been executed.

## Artist workflow

Create > Subdivision Surface creates a smooth cube. The new top-level Subdivision menu provides
creation, cage selection, modeling, surface settings, UV/hull tools and conversions.

1. Create a cube/plane or convert a polygon/voxel object. Conversion retains its source.
2. Choose Vertex, Edge or Face Selection. Edit the low-resolution control cage with the familiar
   click/box modifiers and Move/Rotate/Scale gizmos; the evaluated surface updates from the cage.
3. Extrude, inset, bevel or edit topology using shared modeling previews and Apply/Cancel.
4. Choose level 0..4. Zero shows the cage. Preserve Sharp Edges keeps cage dihedral angles above
   60 degrees sharp. Boundary corners interpolate; other boundary points form smooth curves.
5. Bake a polygon or preview voxelizing the evaluated surface. Voxel-to-subdivision previews
   expose level 0..4 so a large cage can be inspected at a lower level before Apply.

| Workflow | Available behavior |
| --- | --- |
| Selection | Object/vertex/edge/control-face modes; window/crossing, add/toggle/intersect, all/none/invert, connected, grow/shrink. Recovered quad diagonals are hidden from edge picking. |
| Transform | Shared transforms, pivots, snapping, grouping and object workflows; component transforms edit the cage. |
| Modeling | Extrude, inset, face/convex-edge bevel, weld, cage refinement, corner splitting, bridge/cap, slice, join, extract and separate. |
| Surface | Levels 0..4, boundary rules, optional sharp edges, cached evaluation and a cage overlay. Level/sharpness edits support Undo/Redo. |
| UV/materials | Material assignment, saved UV seams/pins and the UV Workspace edit the cage. UV0/UV1, colors, materials and texture bindings transfer to evaluated geometry; source-triangle attribute domains retain UV discontinuities. |
| Booleans | Shared Manifold backend operates on evaluated surfaces. New results are baked polygons; Replace A retains its subdivision settings and previews that further-refined result. |
| Collision | Shared hull fitting/editing uses the cage. Play's cooked polygon path uses evaluated geometry. |
| Conversion | Polygon/voxels to subdivision; subdivision to baked polygon or voxels. Sources are retained. |
| History | Shared geometry/selection transitions. Component clipboard and split/extract outputs retain subdivision settings. |

The overlay identifies the type and level, draws at most 12,000 cage edges and can be hidden.
Component modes pick the cage; object picking and surface snaps pick evaluated geometry.
Contained face boxes require the whole recovered quad; crossing selects a touching quad.

## Representation and limits

EditorObject owns a triangulated control cage plus SubdivisionSettings and has a distinct
geometry discriminator. The polygon backend edits the cage. An evaluated mesh is cached by
cage content hash and settings; level zero uses the original cage.

Consecutive same-material triangle pairs sharing an oppositely wound edge and four distinct
geometric vertices are interpreted as control quads. Pairing depends on connectivity, so moving
a corner out of plane does not change the inferred topology. Other triangles remain Catmull-Clark
control faces. Arbitrarily optimized triangle ordering may infer different quads from an artist's
original mesh. Explicit authored n-gon topology is a future extension.

Exact coincident positions define shared geometric vertices. Face-varying UV/color data remain
separate and are sampled in the original triangle parameter domains, including seams on recovered
quad diagonals. Coincident disconnected shells should be separate objects. Corner splitting
enables separate UV editing; coincident positions still share subdivision geometry.

This is uniform CPU refinement producing a finite triangulated approximation. It does not
evaluate the exact limit surface, implement GPU/adaptive tessellation, editable semi-sharp
weights, displacement sculpting or volumetric brushes. Self-intersections are not certified away.
Normals are accumulated in smooth vertex fans, separated at sharp edges; tangents use the
existing polygon refresh path.

Limits: 50,000 control triangles, 100,000 control attribute vertices, valence 64, 125,000 refined
faces, 500,000 internal geometric points and 64 MiB decoded textures. Output uses up to six
attribute vertices per refined quad to preserve discontinuities. Smoothed evaluation refuses
degenerate faces, inconsistent winding, non-manifold edges, broken boundary fans and vertex-only
shell contacts. Existing viewport triangle budgets still apply. Complex Boolean results may
need baking or a lower level before further subdivision.

## Saving, prefabs and Play

Scene format 9 adds geometry kind 4 and level/sharpness records. Saving writes the original cage
to id.cage.dmesh and the evaluated mesh to id.dmesh. Reopening restores editable cage data.
Older readers cannot open subdivision scenes; scenes without subdivision keep formats 6/7/8.

Cloning and prefab templates retain subdivision settings. Settings participate in prefab content
hashes and override history; template refresh retains explicit level/sharpness overrides.
The shared bake_editor_polygon path uses evaluated geometry for Play and collision spawning.
New conversions preserve placement, grid scale, flags and membership; source scripts, children,
prefab links and socket attachments remain on the retained source.

This batch retains PR #118's conversion routing, palette checks and stale-preview safeguards.

## Reference

[OpenSubdiv subdivision-surface documentation](https://opensubdiv.org/docs/subdivision_surfaces.html)
distinguishes finite refinement from limit-surface evaluation and describes boundary and
face-varying interpolation. This implementation uses finite refinement and adds no OpenSubdiv
dependency.

## Manual handoff (not executed)

- Create cube/plane, inspect levels 0..4 and sharp edges, deform a corner, and Undo/Redo.
  Confirm the logical quad and UV seams remain stable when the corner moves out of plane.
- Exercise vertex/edge/face modes, whole-quad window/crossing boxes and modifiers; ensure
  hidden diagonals are not cage edges. Try extrusion/inset/bevel and numeric Apply/Cancel.
- Exercise weld, refinement, bridge/cap, slice, join/extract/separate and component clipboard;
  confirm subdivision settings survive and geometry/selection Undo restores the prior state.
- Edit UV0/UV1 and corner colors, including an internal diagonal seam. Inspect material groups,
  normals, texture bindings and both cage and evaluated-surface picking.
- Convert polygons/voxels to subdivision, choose the voxel-conversion level, bake polygons and
  voxelize the evaluated mesh. Confirm retained sources, placement, nonuniform scale and Undo.
- Exercise Booleans where the backend exists; compare New baked output and Replace A preview.
  Check budget/topology rejection without changing the source.
- Save/reopen mixed and legacy scenes; inspect cage and evaluated files. Exercise prefab level
  overrides, template refresh and content hashes. Inspect evaluated geometry/collision in Play.
- Check locks, visibility/isolation, narrow menus, overlay/display limits and invalid topology.
