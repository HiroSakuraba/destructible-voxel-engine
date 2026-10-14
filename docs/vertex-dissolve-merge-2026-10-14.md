# Redundant vertex dissolve and protected distance merge

This change builds on PR #125's edge dissolve and unused-attribute cleanup. Both tools work on one visible, unlocked polygon mesh in vertex mode, including subdivision control cages. They preview from a captured source, commit with Apply/Enter, cancel with Escape, and store geometry, authored subdivision settings, UV flags and selection as one Undo/Redo transaction.

## Dissolve redundant vertices

`polygon.dissolve_vertices` and `subdiv.dissolve_vertices` remove selected straight boundary vertices in disjoint two-triangle fans. Each fan becomes one triangle. The midpoint must lie strictly between its boundary neighbours, and its UV0, UV1, normals, colors and tangents must match interpolation along that boundary within relative tolerance 1e-6. Shared fan corners must have compatible attributes and both triangles must have the same material. Surviving attributes remain unchanged.

The tool refuses bent boundaries, interior vertices, fans with more than two triangles, overlapping selections, incident authored seams/creases, discontinuous attributes and pins at retired corners. Surviving corner pins and saved faces receive explicit descendants. This preserves the triangulated control surface and its attribute interpolation to the stated tolerance; editing the control cage can change the evaluated subdivision surface. Preview that surface before applying.

This is deliberately bounded support, not general interior-vertex dissolve, arbitrary polygon retriangulation or n-gon authoring.

## Merge vertices by distance

The existing `polygon.weld` and `subdiv.weld` editor actions now use a protected merge preview. Select geometric vertices, type a finite positive tolerance in **mesh-local units**, or use Up/Down to adjust it. The initial distance is 0.01. Apply is unavailable if the tolerance produces no merge or invalid geometry.

Selected geometric vertices are visited in canonical index order and snapped to the first existing anchor within the distance. Clusters do not chain transitively beyond an anchor's radius. Unselected geometric vertices are never candidate anchors. UV/normal seam copies at a selected geometric vertex move together but retain their separate attribute records. Both UV channels, normals, tangents, colors, material assignments and render triangle order are preserved. Unreferenced attributes are compacted.

Unlike the previous editor weld workflow, this operation refuses collapsed or inverted triangles rather than silently removing them. It also rejects duplicate faces, inconsistent edge winding, edges with more than two incident faces, and disconnected vertex fans (bowtie vertices). It preserves seam markings and pinned corner identities. Creases propagate to the merged geometric edge; conflicting strengths or a retired crease refuse the edit.

The low-level legacy `edit_polygon(..., PolygonEdit::Weld, ...)` API remains unchanged; these protections apply to the native artist/editor actions and the new `merge_polygon_vertices` API.

## Metadata and validation

Operation-local lineage now includes render-triangle descendants, in addition to attribute survivors, geometric-edge descendants, corner descendants and saved-face descendants. Cleanup rebuilds control-face records from those explicit triangle mappings. No nearest-position heuristic is used to recover metadata after cleanup. These indices are scoped to one edit; this does not introduce global persistent topology identities.

Both APIs are bounded to 100,000 attribute vertices, 50,000 render triangles and 2,048 selected geometric vertices. The input and output must be consistently wound manifold triangle meshes. The batch is refused as a whole if any selected dissolve is unsupported or if a merge is invalid. Preview Apply rechecks the saved UV authoring record before constructing the transaction.

Core regression coverage includes redundant boundary removal, dual-channel interpolation, surviving and retired pins, face mappings, material boundaries, exact seam-copy retention, crack-edge joining, selection anchors, crease propagation/conflicts, invalid tolerances, stale selections, no-op merges, collapsed faces and bowtie refusal. Native tests cover preview/Cancel, Apply, exact Undo/Redo of geometry/settings/flags/selection, locks, isolation and typed no-op tolerances.

Local validation: compiled and ran the focused core regression executable, syntax-checked changed native sources and integration tests, and checked the source manifest. Full native runtime execution is unavailable in this environment because PNG/JPEG development dependencies are missing. No interactive visual acceptance run was performed.

Manual acceptance: in Edit mode select the midpoint of a straight two-triangle boundary, invoke Dissolve Redundant Vertices, compare both UV channels, Cancel, then Apply/Undo/Redo. For merge, select both endpoint pairs of a narrow crack, invoke Merge by Distance and raise/lower the tolerance until the gap joins. Check seam overlays and crease strengths before and after Undo. Verify selecting only one crack endpoint pair refuses a disconnected fan; adding a pin to the dissolved midpoint must prevent Apply.
