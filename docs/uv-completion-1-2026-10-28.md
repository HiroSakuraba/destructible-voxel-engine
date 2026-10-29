# UV-Completion-1 — native UV authoring completion
28 October 2026 · scope: ART-074, ART-147–149, SUBD-135

This batch builds on the merged native UV workspace (PR #113) and saved UV seams/pins (PR #116). It does not change the mesh geometry data model and intentionally excludes NURBS/curvature work.

## What changed

### Texel density measurement and matching

Select complete UV islands (UV Workspace, **4 Island**, or select all with A). **B** measures texels/metre from the UV triangle area divided by physical surface triangle area, including nonuniform object-axis scaling. The current material's base-color image dimensions are used when present; otherwise the editor uses a visible 1024×1024 reference. Non-square textures use the geometric mean of U/V texel dimensions (equivalent-area density). Degenerate UV charts, zero physical area and partial islands are refused. Density is reported over all selected islands, and the API also exposes each island separately.

**Shift+B** copies the measured aggregate density to the matching target. **I** allows typing a positive target density (texels/metre), with Enter to accept and Esc to discard. **N** previews matching **each** selected complete island to that density independently; its aspect ratio is unchanged and each island remains centered. It does not pack or eliminate existing overlaps. Existing saved UV pins within selected islands must be cleared first; the tool does not silently move constraint locations.

### UV island alignment and straightening

**O** aligns each selected whole island's longest UV edge with U (horizontal). **L** aligns it with V (vertical). Both are rotations about the island centroid and preserve individual UV triangle areas and shapes; this is not the same as rectangular packing. For partial UV component selections, **C** straightens horizontally (constant V), and **Shift+C** vertically (constant U). Outside selected corners are detached from shared UV attributes before editing, so other faces/channels are unchanged. Refuse selected pinned corners for automatic straightening.

**N, O, L and C** create a preview. **Enter** applies it as a single undoable ReplacePolygonCommand; **Esc** cancels and restores the mesh. The original UV workspace still provides G/R/S and typed transform previews.

### Seams and pins

Saved seams remain bright red even when selected. Saved pins now display as purple bordered squares in Vertex, Edge, Face, and Island modes instead of vanishing outside Vertex mode. A legend shows both counts and the active UV channel. Saved seam/pin authoring and editor-only component serialization are unchanged; both channels remain independent.

### Subdivision UV smoothing

Subdivision menu now offers **UV Smoothing: Linear** (previous exact piecewise-linear UV interpolation) and **UV Smoothing: Smooth Interior (Keep Seams)**. The second option performs a conservative single-step UV relax on evaluated attribute vertices only: coincident positions and identical UVs form shared UV nodes, a chart edge appearing once is a fixed boundary, and only interior nodes with at least three neighbors relax by one-quarter toward neighbor mean. Geometry, material assignments, normals and source cage are untouched; both UV channels are handled independently. This is a bounded *relaxation* mode, not a fully certified OpenSubdiv face-varying Catmull-Clark implementation.

The change is undoable via SetSubdivisionSettingsCommand and serialized in **editor-scene format 15** only when a non-default smoothing mode is present. Earlier scene formats (including 10 and 14) remain readable and their default UV mode is Linear. Prefab templates/overrides preserve the choice.

## Implemented regression tests

- Original UV0/UV1 density on a scaled polygon plane, chart completeness enforcement, target-density matching, geometry and other-channel invariance.
- Island U/V orientation paths, triangle-area preservation, UV pin refusal, per-corner straightening and isolation.
- Linear vs smooth-interior subdivision mesh evaluation, unchanged geometric positions and topology, format-15 save/reopen, invalid mode refusal.
- Existing UV seams and pins tests cover undo/redo, metadata validation, scene round-trip, and unwrap.

## Acceptance still required

No local compilation, GPU/viewport capture, or windowed artist acceptance was run through this GitHub connector. A green, executed Linux/Windows CI matrix is required before merge. On a checker-textured **polygon** and **subdivision** sample with seams, confirm:

1. UV0/UV1 independent density, target 128/512/1024 texels/m, nonuniform scale, unequal base-color image dimensions and clear error on partial islands.
2. One island rotated along U, another along V, C/Shift+C straightening, no movement of unselected charts, seam/pin locations retained, and cancellation/Undo/Redo/save/reopen.
3. Selected UV channel and seam/pin overlay after switching Vertex, Edge, Face and Island; zoom, pan and text contrast at narrow viewport widths.
4. Subdivision Linear versus Smooth Interior on UV islands with explicit seams, boundary preservation, texture distortion and absence of UV foldover. Inspect exported/baked mesh.
5. Export strips editor-only seam/pin flags while preserving edited UV coordinates. Prefab template/instance overwrite and local smoothing-mode override round-trips.

Outside this batch: UDIM management, atlas export, global optimal packing, automated stretch/overlap heat maps and true face-varying UV subdivision rules.

## Test targets

Run `dve_editor_mesh_authoring_tests`, `dve_editor_subdivision_tests`, editor scene/prefab suites, and full CI. If a target is renamed or unavailable, use `ctest --test-dir <build-dir> -N` to discover the configured name.
