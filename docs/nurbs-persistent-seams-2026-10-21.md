# Persistent paired NURBS seams and shared display samples

Follow-up: [paired CV gizmos and background display](nurbs-paired-cv-drags-2026-10-21.md) supersedes the linked-gizmo and synchronous-drag restrictions below. The remaining placement, parameterization, copy/prefab and multiple-seam limits still apply.

This follow-up builds on the boundary-matching batch in PR #170. It adds an explicitly limited persistent constraint: one linked seam per patch, connecting two untrimmed native surfaces in the same parent/pose/axis-scale frame, without socket attachments. These are independent scene objects, not a sewn solid or a general patch graph.

## Artist workflow

1. Use the existing NURBS boundary choices and optional reversed correspondence. The active patch is the reference. If necessary, use Match Other Boundary first to create an aligned patch in the reference frame.
2. Select the reference and other patch, then choose Link Selected Boundaries (C0 or C1, Shared Samples). Both objects are retained; the moving patch is matched in place. The reference may be refined/elevated without refitting. One undo step restores both pre-link sources and caches.
3. Numeric CV/weight edits on the reference, free CVs on the moving patch, UV-range resets and fixed/adaptive display changes rebuild both patches in one history entry. The moving boundary row, and its tangent row for C1, are driven by the reference and refuse direct edits.
4. Rebuild Linked Seam Pair resamples explicitly. Unlink Seam removes the relationship from both objects, keeps their current matched source geometry, and restores independent display policies. Undo/redo includes both objects and reciprocal metadata.

This first version deliberately refuses linked CV gizmo drags, independent patch placement/reparenting, parameterization changes, duplicate/copy and prefab operations. Unlink first. Both patches must be visible, unlocked, owned and available to the native edit. Delete both patches together, or unlink before deleting one. Ordinary standalone NURBS behavior is retained. General graph drags, hierarchy-aware transforms, multiple seams on one patch and propagated knot changes remain follow-ups.

## Display and continuity contract

The seam uses C0 position or homogeneous C1 derivatives in the normalized cross-boundary charts established by #170. Persistent matching anchors the common rational weight gauge at the boundary, so repeated rebuilds do not repeatedly rescale the moving patch's free weights. The legacy one-shot matching policy remains unchanged unless the stable-gauge option is explicitly requested.

Each patch starts from its selected display policy. Fixed display generates per-span parameters. Adaptive display exposes its actual tensor grid and accepts a validated seed grid, preserving the existing sampled chord/normal checks and bounded refinement. The paired builder unions tangential seam samples, maps correspondence, seeds both policies and repeats until neither grid changes, up to 12 rounds. It refuses control, vertex, sample, precision and convergence budget failures before publication. Explicit grids retain every source knot and use one-sided corner derivatives.

Affine parameter reversal can produce adjacent floating-point representations of the same sample. The scheduler coalesces nearby representations only when both boundaries evaluate to the same float position. Each mapped grid must still contain every authored source knot, with strictly increasing, representable spans; authored knots are not silently removed. Distinct geometric samples remain separate or cause a precision refusal. This display-sample handling is separate from the exact knot-union policy of source compatibility preparation.

The reference boundary is evaluated once for canonical float positions. Both meshes use exactly those positions at every scheduled seam vertex. Under the required common frame, the paired meshes therefore have matching geometric edge vertices and no tessellation T-junction across that seam. Normals/UVs remain per patch; shading continuity, displacement shaders, welding and solid topology are not implied. Snapped triangles are checked for float-precision collapse.

The explicit-grid path is bounded to 65,536 duplicated cell-corner vertices per patch. Shared sampling can exceed the budget even when each independent mesh fits; the entire command then fails. Paired edits are synchronous in this first version. The existing standalone background-CV worker refuses linked snapshots, and native linked drags are disabled until a graph-aware worker can own both snapshots safely.

## Persistence and validation

An optional `seam` property on each `dve.nurbs` component stores a versioned `DVE_NURBS_SEAM 1` record: both object IDs, directed boundaries, correspondence/C1 policy, both source stamps, that object's U/V grid, canonical seam fractions and common float positions. Counts, finite numbers, endpoints, ordering, object ownership and trailing data are checked. Metadata is bounded to 1 MiB and 4096 parameters per axis/seam. Source codecs remain v2/v3; linked caches require an engine that understands the seam property.

Per-object validation deterministically regenerates the constrained display, checks its source stamp and reference anchors, and compares cache geometry, normals/tangents and UVs. Document validation additionally checks reciprocal partners, common frames, matching stamps/schedules/anchors and sampled position/C1 continuity, including midpoints. Sampled continuity and adaptive errors are numerical checks, not certified interval bounds.

Coordinated commands stage both source/cache replacements before publishing either. Undo/redo rejects stale source, geometry, placement or link state, preserves separately edited material resources, and adds no partial history on failure. Generic transform, scale, reparent, source-component removal/disable, copy/add and deletion paths guard against orphaning a seam. Prefab capture/instantiation is refused until linked-object ID remapping exists. Runtime scene export already strips the complete editor-only NURBS component and uses the cooked meshes.

## Autodesk reference points

[Maya stitchAndExplodeShell](https://help.autodesk.com/cloudhelp/2025/ENU/Maya-Tech-Docs/Commands/stitchAndExplodeShell.html) identifies differing edge vertices during tessellation as a cause of rendered NURBS cracks. DVE addresses that geometric sampling issue for a paired common-frame seam; it does not implement Maya's shell, trimming or dependency-node architecture.

[Softimage Modifying Surface Meshes](https://download.autodesk.com/global/docs/Softimage2014/en_us/userguide/files/surfmesh_ModifyingSurfaceMeshes.htm) documents a surface continuity manager, boundary-point editing policies and knot-curve propagation across junctions. DVE now has a retained reference-driven constraint and paired history. Full assembled meshes, multiple junctions, exclusions, direct boundary dragging and propagated knot edits remain missing.

## Validation

18 selected local CTest suites passed: persistent seams; attachment; background jobs; adaptive display; modeling; length; degree; boundaries/join; display; topology; construction; control tools; NURBS core/editor; native polygon; subdivision; scene export; chrome fit; and generic editor commands. A focused AddressSanitizer run with the seam/grid sources and dedicated test instrumented also passed (leak detection disabled).

The dedicated seam suite covers all 16 edge pairings, both directions and C0/C1; bit-identical boundary vertices; different fixed densities and adaptive policies; propagation after numeric edits; driven-row and nonpositive-weight refusal; repeated weight-gauge stability; stale-source/cache and malformed-metadata refusal; save/reopen; unlink/history; locked/hidden partners; independent placement, duplicate/prefab and single-delete refusal; paired deletion/undo; and shared-budget failure. Headless tests do not establish windowed/GPU artist acceptance.
