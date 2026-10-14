# Edge loops and rings on authored subdivision cages

This first everyday-modeling batch adds selection tools to the existing subdivision
workflow. It does not modify mesh topology or introduce another geometry type.

## Artist workflow

1. Select a subdivision object and enter **Subdivision > Edge Selection**.
2. Click an edge, or select several seed edges with the existing click/box modifiers.
3. Choose **Select Edge Loops** or **Select Edge Rings** in the Subdivision menu or
   Command Center. The Polygon menu also exposes the shared cage commands; ordinary
   polygon objects show a disabled reason instead of guessing authored quads.
4. The selection becomes the union of the chains through all selected seed edges.
   The status line reports selected-edge count and boundary, pole or triangle stops.

A loop follows connected edges through regular four-edge quad vertices. Along an open
surface boundary it follows the other boundary edge at a regular three-edge vertex;
it stops at a boundary corner or where an interior chain reaches the boundary.
A ring crosses the opposite edge of each authored quad. Closed loops and rings terminate
after visiting the cycle, without relying on a geometric angle heuristic.

Both tools use the saved control-face records, excluding hidden render-triangle diagonals.
Split UV/normal attribute vertices use the editor's existing geometric-position identity
convention, so the traversal can cross attribute seams without modifying UVs or normals.
Face order and render-triangle order do not define the chain.

Invalid metadata, stale/diagonal seeds, degenerate faces, inconsistent winding,
nonmanifold edges and touching-shell vertices refuse the operation. Triangle faces and
non-regular quad vertices stop a supported chain; the tool does not branch arbitrarily.
The cage budget is 50,000 triangles / 100,000 attribute vertices; selection is bounded
by 150,000 edges. Budget exhaustion returns no partial result or selection replacement.
Play, locked/hidden/isolated-out objects, active transforms and authoring previews block
the native commands. Selection expansion uses the existing selection path and does not
add a geometry Undo entry or change the scene format, control faces, creases or UV flags.

## Validation in this PR

- Compiled and ran the core regression executable with GCC 13.3 in C++23 mode, using
  the actual subdivision, polygon, asset-validation, transform and material code.
- Core checks passed for open strips, boundary chains, closed cylinder loops/rings,
  a cube ring and cube pole stops, seam-split attribute vertices, multiple seeds,
  repeated expansion, reordered saved faces/triangles, hidden diagonals, stale seeds,
  malformed/missing metadata, triangle stops, nonmanifold refusal and selection budgets.
- Syntax-compiled changed native controller, polygon controller, workspace and the
  complete test source (including the native integration checks).
- Full CMake configuration was blocked by missing PNG development libraries. The
  CMake-registered native integration checks have not been executed locally; normal
  repository CI and artist acceptance remain to verify them.

The independently runnable core regression command, from the repository root:

```sh
g++ -std=c++23 -O0 -ffunction-sections -fdata-sections \
  -DDVE_CAGE_SELECTION_CORE_ONLY -Iinclude \
  tests/test_editor_cage_edge_selection.cpp \
  src/editor_subdivision.cpp src/editor_polygon.cpp src/polygon_asset.cpp \
  src/transform.cpp src/master_material.cpp src/material_mapping.cpp \
  -Wl,--gc-sections -o /tmp/dve-cage-edge-selection-tests
/tmp/dve-cage-edge-selection-tests
```

In a configured build with the editor dependencies, build and run
`dve_editor_cage_edge_selection_tests` without `DVE_CAGE_SELECTION_CORE_ONLY` to include
native menu dispatch and locked/hidden/isolation/selection-mode guards.

## Next implementation batch

Stable operation-specific identities and edit-lineage mapping must accompany loop cut
and slide, before those operations move or reindex vertices. The new traversal is read-only
and deliberately reuses the current identity convention; it does not solve that migration.
Coincident disconnected vertices retain the existing convention and can be refused as
touching or nonmanifold topology rather than being silently repaired.

Next: shared quad-strip adjacency for cut previews, explicit split/survivor mappings,
per-corner UV interpolation, inherited crease/seam flags, then one cut and clamped slide
with Apply/Cancel and atomic geometry/selection/metadata history. General dissolve,
concave/open bevels, UV usability, selection sets/soft selection and an interactive Slice
plane remain the later worklist batches.

For artist acceptance, try a subdivided plane, an open quad cylinder and a mixed
triangle/quad cage. Confirm the status messages match the visible chain stops and the
selection never includes an internal triangulation diagonal. Try the commands from
both menus and Command Center while a preview is open and with locked/hidden geometry.
