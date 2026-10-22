# Connected NURBS seam networks

Surface patches can now have one seam on each of their four boundaries. Link C0/C1 adds an incident relationship to the existing connected component; Rebuild Seam Network rebuilds that whole component. Unlink Patch Seams removes every seam incident to the selected patch and retains other relationships, source shapes and owned object IDs. A link that combines components, an edit, unlink or refresh is one atomic command covering all affected objects.

The supported driving graph is directed and acyclic. References drive moving boundary rows, plus adjacent tangent rows for C1. Branches and converging paths, including a four-patch corner junction, are supported when the constraints agree. A directed dependency cycle, reused boundary or inconsistent incoming constraint is refused before publication. This is deterministic constraint propagation and validation, not an iterative best-fit solver for conflicting networks. Boundary identity, twist correspondence and the normalized-chart homogeneous C1 policy are inherited from boundary attachment.

## Geometry and shared display

Compatibility prepares exact tangential degree/knot unions in a stable topological order. Propagating basis changes across the component is bounded to 17 passes. After applying constraints, every seam is checked against the final sources, so a later incoming constraint cannot silently break an earlier one.

Each patch retains its fixed/adaptive display policy and one tensor grid shared by all its incident seams. Edge sample schedules propagate across the component and seed adaptive refinement until the grids stop changing, within 32 rounds. Source knots, representable spans and existing per-patch vertex limits remain enforced. Shared endpoint groups choose a canonical object/corner root; all incident displays use the same float position at that junction. Interior seam samples likewise use shared canonical positions, while UVs and normals remain per patch. Continuity and approximation checks are numerical samples, not certified interval bounds.

Limits: 16 patches and 24 seams per component, one seam per boundary, 4096 samples per axis/seam, 65,536 duplicated cell-corner vertices per patch and 524,288 across a network. Every patch must share the same parent, pose and axis scale, with no socket attachment. Budget, precision, singularity and convergence failures leave the live component unchanged.

## Persistence, editing and history

The optional `dve.nurbs.seam` property remains the entry point. Single incident seams retain `DVE_NURBS_SEAM 1`; multiple incident seams use a bounded `DVE_NURBS_SEAMS 2` container of two to four quoted version-one records. Each record retains source stamps, that patch's common U/V grid, edge fractions and anchors. The source codecs stay v2/v3. Existing pair scenes remain readable; multi-seam scenes require this updated engine.

Scene validation checks every reciprocal relationship, occupied boundary, component size, driving cycle, source stamp, grid, sampled continuity and canonical junction anchor. Generic transform/reparent, duplicate/copy, prefab, source removal/disable and partial deletion guards continue to prevent orphaned relationships. Whole-network deletion and undo are supported. Hierarchy-aware network transforms, copy/prefab ID remapping and user-directed knot/degree changes remain separate work.

Numeric CV/weight and display-policy commands rebuild the connected component synchronously. Any selected moving control driven by any incoming seam is read-only. Gizmo jobs capture the entire component on the existing cancellable worker, including distant patches; generation, source/cache, placement, visibility/lock, ownership, isolation and revision guards cover every member. The last complete network remains visible until the newest complete result is ready. Source/metadata/cache publication stages all replacements before installing any. Cancellation restores surviving unmodified members after a remote deletion and preserves external source/cache edits and live material resources. Snapshots keep 1x1 image placeholders, following the paired-worker resource policy.

A successful drag adds one cached network undo/redo step. Identity drags add none. Escape, focus loss and Undo during a drag cancel the preview; autosave and conflicting editor commands retain the existing drag protections. No worker/job state is serialized.

## Validation

All 20 selected headless CTest suites passed. The dedicated suite covers polynomial/rational four-patch junctions, C0/C1 and mixed approximation, different link insertion orders, reversed/swapped axes, a four-boundary hub, consistent propagation, conflict/cycle/occupied-boundary refusal, repeated refresh, network budgets, reciprocal metadata, atomic history, unlink/splitting, whole-network deletion, background generations, distant lock/source/cache/deletion/material changes and save/reopen. Existing pair, worker, geometry, generic editor, export and menu tests remain part of the selected regression set.

Headless checks do not establish windowed/GPU performance, artist acceptance, topological sewing or complete Maya/Softimage parity. Closed cyclic constraint solving, trims, G2/C2, periodic patch networks and general surface fillets remain future work.

A focused AddressSanitizer run passed with the network test, seam/network builder, worker queue, native NURBS controls/transform and editor-command sources instrumented. Other engine archives were uninstrumented; leak detection was disabled.
