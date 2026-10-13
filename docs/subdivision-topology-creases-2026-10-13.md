# Subdivision control topology, creases and cage workflow

Follow-up to PR #119. Source review only. No builds, tests or CI were run, and the manual
acceptance steps below have not been executed. Publication uses `[skip ci]`.

## Delivered controls

The Subdivision menu adds Make Selected Control Quad, Triangulate Selected Control Faces,
selected-edge crease strengths 0.5, 1, 2, 3 and 4, Clear Selected Edge Creases, Cage X-ray
Selection and Preview Selected Cage Only.

Control faces are saved triangle/quad records referencing oriented attribute-vertex triples.
Evaluation resolves those triples against the current cage instead of repeatedly pairing
adjacent triangles. Reordering triangles with their material assignments preserves the saved
face groups. Quad selection and hidden-diagonal edge selection use the same records.

Make Selected Control Quad accepts exactly two adjacent, consistently wound, same-material
triangles. Triangulate Selected Control Faces exposes their diagonals as real cage edges.
These commands change the subdivision topology without moving cage vertices or altering UVs.
They evaluate the proposed surface before committing, support Undo/Redo, and remove a stored
crease if its edge becomes a hidden diagonal.

Crease strengths blend smooth and crease rules. Strength is reduced by one per refinement;
0.5 blends the first refinement and 4 remains sharp through four refinements. Two creased
edges blend toward the crease vertex rule; three or more blend toward an interpolated corner.
Boundary edges remain sharp, with the previous boundary-corner convention retained. The
existing global 60-degree sharp-edge option remains available and can mask weaker authored
creases; disable it when judging semi-sharp strengths. This is uniform CPU semi-sharp
refinement, not a claim of bit-for-bit OpenSubdiv equivalence or exact limit evaluation.

Purple cage edges indicate authored creases; selected edges remain orange. The viewport
caption reports the selected crease-strength range. Cage X-ray controls component click and
box selection independently of the global viewport X-ray option. If global X-ray is enabled,
it also permits selection through geometry. The existing cage overlay remains a visible guide.
Cage-only preview displays the selected cage in the software viewport without changing its
saved subdivision level. Component picking always targets the cage; object-level picking,
surface snaps, conversion and Play continue to use the evaluated surface. This preview does
not change runtime render settings.

## Editing, history and persistence

Creation and legacy loading freeze the initial triangle/quad grouping once. Geometry history
commands retain before/after subdivision metadata, including merged component-transform
strokes. Live extrusion/inset/bevel/Slice previews restore cage and metadata on Cancel.
Extraction, shell separation, slicing and component clipboard remap surviving faces and
creases to their output cages. Mesh-authoring and Boolean previews use remapped metadata.

When indices and material assignments are unchanged, face/crease identities survive cage
deformation. Other topology changes match surviving oriented triangles by exact position,
preserve valid old groups, split material-incompatible quads, and infer newly emitted control
faces once. Creases survive when both old endpoint positions remain as a control edge.
Deleted/replaced edges lose their crease. There is no invented crease propagation across
new extrusion/bevel/slice edges. Bulk topology edits that simultaneously relocate and reindex
vertices can lose old face groups/creases; operation-specific stable vertex IDs remain future
work. UV/color discontinuities still use the source triangle attribute domains.

Scene format 10 stores bounded control-face and crease records. Format 9 scenes load and
freeze their old pairing; scenes without subdivision continue to use earlier scene formats.
Older editors cannot open format 10. Invalid/missing face coverage, incompatible quads,
out-of-range indices, duplicate crease keys, nonfinite strengths and size-budget violations
are refused. These checks also apply at level zero.

Prefab hashes include effective face topology and crease weights. Legacy hashes are accepted
only for template scenes older than format 10; new metadata cannot bypass integrity checks.
Crease/topology edits mark the existing polygon.geometry override scope, so prefab refresh
preserves the instance cage and settings. Undo restores prior override state. Cloning retains
metadata and discards evaluated caches.

## Manual acceptance handoff (not executed)

- Create a cube and plane. Triangulate a selected quad, select its diagonal, then restore the
  two triangles with Make Selected Control Quad. Check whole-face box selection and Undo/Redo.
- Reorder cage triangles while preserving material assignments; confirm stored quads, hidden
  diagonals and evaluated triangle-to-control-face mapping remain stable.
- Set edge strengths 0.5 through 4 with the global sharp toggle disabled. Check isolated darts,
  two-edge creases, three-edge corners, boundaries, normals, highlighting and Undo/Redo.
- Compare component clicks/window/crossing boxes with cage and global X-ray on/off. Toggle
  cage-only preview; confirm saved level, conversion output and Play geometry do not change.
- Exercise extrusion, inset, bevel, Slice, extraction, shell separation, component clipboard,
  Booleans and repeated preview Update/Cancel/Apply. Confirm surviving identities and history.
- Save/reopen format 10, migrate a format 9 scene/prefab, and exercise prefab geometry overrides.
  Inspect malformed metadata rejection without mutating source objects.

## Remaining work

Explicit n-gons, editable corner weights, operation-specific stable topology IDs and crease
propagation, quad-aware loop cuts, adaptive/GPU evaluation and UV-boundary smoothing modes
remain separate batches. The shared polygon modeling backend is still triangulated.
