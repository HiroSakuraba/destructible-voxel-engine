# Protected edge dissolve and unused-vertex cleanup

This batch starts the cleanup priority after merged PRs #123 and #124. It adds two
native menu/Command Center actions and uses the existing geometry/selection history.

## Dissolve selected cage edges

Select edges on a subdivision cage, then choose **Dissolve Selected Cage Edges**.
The preview joins each disjoint pair of adjacent saved triangle faces into one explicit
control quad. Enter/Apply commits one history step; Escape/Cancel and focus loss restore
the original topology and selected edges. Apply revalidates saved UV seams before committing.
The merged control faces become selected after Apply. Undo restores the original triangles
and seed-edge selection; Redo restores the authored quads.

The operation removes an edge from the authored cage without deleting the surface's render
triangles, vertex attributes or material assignments. Saved UV pin corner numbers and UV
layout hashes therefore stay unchanged. Unaffected control-face identities map to their
survivors; each pair of dissolved triangles maps to one common child face, and the removed
edge is explicitly retired. Existing prefab geometry override behavior remains in use.

The first supported cases are disjoint triangle pairs forming convex planar quads. The tool
refuses boundaries, selected pairs sharing a face, stale/hidden edge keys, nonplanar or
concave pairs, different materials, saved UV seams in either channel, authored creases and
UV/color/normal discontinuities. Disabled UV authoring components still protect saved seams.
There is no partial application if any selected edge is unsupported.

Merging two existing quads can produce a six-sided face; that requires larger authored-face
support and is deliberately refused. The evaluated subdivision surface may change when two
triangular control faces become a quad, even though the underlying cage geometry is retained.
The preview lets the artist judge that change before committing.

## Clean unused mesh/cage vertices

**Clean Unused Mesh Vertices** (Polygon) and **Clean Unused Cage Vertices** (Subdivision)
remove only attribute vertices that no render triangle references. These include orphaned
vertices retained by earlier modeling operations. Used UV/normal seam copies are never welded.
The command reports the removed count and commits one reversible history step. If there is
nothing to remove, it does not add a dirty edit or Undo entry.

The operation copies surviving attributes exactly, preserves triangle/corner ordering and
materials, updates indices, recalculates bounds from surviving positions, and recomputes the
content hash. It does not regenerate normals or tangents. Explicit survivor maps update
control-face records, geometric selections, crease keys and independent UV seam/pin records.
An unused old canonical vertex at the same position as a used seam copy maps to the surviving
geometric vertex rather than disappearing from an otherwise valid edge selection. A genuinely
unused selected point is retired. Unrelated components remain intact; the saved UV component
keeps its enabled/disabled state.

Geometry, authored topology, UV records, prefab overrides and selection restoration participate
in the existing CompoundCommand history path. Locked, hidden or isolated-out objects, Play,
active transforms and authoring previews refuse cleanup. Input is bounded to 100,000 attribute
vertices and 50,000 triangles for this initial cleanup path.

## Scope still remaining

- Dissolving referenced collinear vertices or general edge chains.
- Dissolve output with more than four authored corners.
- Merge-by-distance cleanup with explicit cluster previews and policies for boundaries,
  UV/color domains, crease conflicts, degenerate faces and connected shells.
- Persistent global element IDs independent of array order, and general modeling lineage.

The unused-vertex command is storage/topology housekeeping; it is not a replacement for those
modeling tools, a mesh repair solver or a claim that all cleanup work is complete.

## Validation

Local core regressions compile and pass with GCC 13.3/C++23 using the actual editor geometry,
subdivision, UV remapping, asset-validation and material sources. New checks cover disjoint
triangle-pair dissolve, explicit edge retirement/face merging, seam/crease/material/UV1 and
shape refusals, orphan canonical aliases, exact surviving corner attributes, unchanged
triangle ordering, independent disabled UV records and crease/control-face compaction.
Existing loop/ring and cut/slide core cases run in the same executable.

Changed editor sources and the full regression source syntax-compile locally. Native tests
are registered in the existing CMake target for dissolve preview/Apply/Cancel and cleanup
geometry/selection/metadata Undo/Redo, protected UV1 seams, locked objects and no-op history.
Those new native runtime cases were not executed locally because PNG development libraries
still block full CMake configuration. CI and manual artist acceptance remain necessary.

```sh
g++ -std=c++23 -O0 -ffunction-sections -fdata-sections \
  -DDVE_CAGE_SELECTION_CORE_ONLY -Iinclude \
  tests/test_editor_cage_edge_selection.cpp \
  src/editor_subdivision.cpp src/editor_polygon.cpp src/editor_uv.cpp \
  src/polygon_asset.cpp src/transform.cpp src/master_material.cpp \
  src/material_mapping.cpp -Wl,--gc-sections -o /tmp/dve-cage-cleanup-tests
/tmp/dve-cage-cleanup-tests
```

For acceptance, triangulate a plane's control faces, dissolve several disjoint diagonals,
inspect the evaluated surface and exercise Cancel/Undo/Redo. Mark a crease and UV0/UV1 seam
on a selected diagonal and confirm refusal without deleting either flag. After a modeling
operation that leaves unused vertices, clean them and verify UVs, materials, creases, face
selection, save/reopen and prefab overrides. Repeat cleanup and confirm no extra Undo entry.
