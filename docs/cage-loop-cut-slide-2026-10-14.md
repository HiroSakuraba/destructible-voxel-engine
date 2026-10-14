# Loop cut and slide with explicit edit lineage

Follow-up to PR #123. This batch adds a single cut across a saved quad strip, then
interactive sliding of that new cut before Apply. It also establishes operation-specific
lineage for geometry and saved authoring metadata. It does not introduce globally persistent
element IDs or change the scene format.

## Workflow

1. Select a subdivision object, enter Edge Selection and choose one seed edge.
2. Choose **Subdivision > Loop Cut and Slide** (also in Command Center and the shared
   Polygon cage commands). The default preview cuts halfway along the seed edge ring.
3. Drag left/right in the viewport, use Up/Down in 0.01 increments, or type a fraction
   between 0.001 and 0.999. Fraction direction follows the seed's canonical endpoint order
   and is propagated consistently across the strip; it is not a world-space distance.
4. Press Enter/Apply to commit one geometry/selection/metadata history step, or Escape/Cancel
   to restore the original cage. Focus loss cancels the preview. Saving and other modeling
   commands are blocked while the preview is open.

Each crossed edge gets one geometric split point shared by its incident faces. Attribute
vertices at UV/normal seams remain separate. Every affected quad becomes two explicit control
quads; new transverse edges form the selected loop. Changing the preview fraction slides this
new loop with the same generated connectivity. Attribute interpolation follows each original
face side, retaining separate UV0/UV1 and color domains.

## Supported scope

- One cut across a complete open or closed ring of convex planar quads.
- Unsupported neighboring triangles, nonplanar/concave faces, self-crossing strips,
  conflicting orientation, internal attribute splits and coincident split points refuse
  the edit. The preview reports the reason and leaves the source visible.
- The existing cage and evaluated-surface budgets still apply. Endpoints are excluded to
  avoid a zero-width row. Existing topology checks validate winding and manifold incidence;
  this is not a general geometric self-intersection certification.
- Dragging slides the **new cut before Apply**. Sliding arbitrary existing loops, multiple
  cuts, edge-distance spacing and snapping the slide are later extensions.
- Refining a subdivision cage can change its evaluated smooth shape. The tool preserves
  supported cage boundaries/attributes, not an exact prior subdivision limit surface.

## Identity and metadata handling

`PolygonTopologyLineage` records old attribute-vertex survivors, geometric edge descendants,
triangle-corner descendants and control-face descendants. Surviving attribute vertices keep
their array indices; new ones append. Explicit child-face records are emitted by the operation,
instead of inferred from newly generated render triangles.

Split edge children inherit the parent's authored crease strength. This inheritance does not
apply subdivision evaluation's semi-sharp decay; that remains an evaluation rule. New transverse
edges start smooth. UV seams follow split-edge descendants independently in each channel.
Pinned old corners follow their emitted corner copies at the same UV coordinates; new split
points are not automatically pinned. A pin shared across the two triangles of a control face
conservatively follows all emitted copies of that unchanged attribute corner. A retired seam
or pin without supported lineage refuses the operation rather than silently deleting metadata.
In particular, a saved seam on a touched quad's hidden diagonal must be resolved first.

`ReplacePolygonCommand` accepts validated, operation-authored after-subdivision settings.
Existing callers retain their original remapping behavior. The cut preview commits the explicit
settings and saved UV record together in a CompoundCommand; Undo/Redo restore the entire cut
and its original seed/new-loop selection. Existing prefab geometry and component-property
override paths remain in use.

This is a bounded lineage foundation, not completion of the worklist's persistent-ID goal.
Global IDs independent of vertex arrays, general merge/dissolve/Boolean lineage and selection
set migration still require their own operations and storage design.

## Validation

The focused core regressions compile and pass locally using GCC 13.3/C++23 and the real
subdivision, polygon, UV remapping, asset-validation and material sources. They cover open
and closed cuts, oriented sliding, explicit face output, inherited crease strengths, independent
UV seams/pins, unchanged pinned coordinates, deleted-seam refusal, malformed input and
unsupported strip geometry, in addition to the loop/ring tests from PR #123.

Changed native controller, polygon controller, workspace and complete regression source
syntax-compile locally. Full local CMake configuration is still blocked by missing PNG
development libraries. The CMake-registered test now includes native drag/Apply/Cancel,
focus-loss cancellation, save/reopen and geometry/selection/metadata Undo/Redo checks;
those runtime integration checks have not been executed locally.

Reproduce the core check from the repository root:

```sh
g++ -std=c++23 -O0 -ffunction-sections -fdata-sections \
  -DDVE_CAGE_SELECTION_CORE_ONLY -Iinclude \
  tests/test_editor_cage_edge_selection.cpp \
  src/editor_subdivision.cpp src/editor_polygon.cpp src/editor_uv.cpp \
  src/polygon_asset.cpp src/transform.cpp src/master_material.cpp \
  src/material_mapping.cpp -Wl,--gc-sections -o /tmp/dve-cage-cut-tests
/tmp/dve-cage-cut-tests
```

For artist acceptance, try a cube and a planar quad grid, mark a crossed crease and a UV
seam, then move the preview to both sides of center. Verify the whole visible strip slides
consistently, new loop selection excludes hidden diagonals, untouched faces retain materials,
UV seams remain split, pins keep their UV coordinates, and Cancel/Undo/save/reopen restore
the expected authoring state. Confirm a mixed triangle/quad strip refuses rather than making
a T-junction. Start a cut after an Inset/Bevel preview to check that arrows adjust the cut
fraction rather than stale controls from the previous tool.
