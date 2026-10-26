# Remaining NURBS batches

Requested sequence:

1. Network transforms and reuse: whole-component placement and grouping, complete-network duplicate/copy with endpoint ID remapping, prefab capture/instances and atomic history.
2. Network refinement: user-directed knot insertion and degree elevation propagated across incident seams while preserving shape and continuity.
3. Persistent curves on surfaces: editable UV curves, retained projection relationships and updates after surface editing.
4. Arbitrary trimming: oriented closed loops, holes, selected regions, trim-aware tessellation, untrim and undo.
5. General surface intersections: bounded numerical tracing of curved branches, boundary/tangency diagnostics and validated trimming curves.
6. Surface blends, fillets and offsets: transition strips, tangent/curvature controls, rolling-ball fillets and trimming support.

The latest user instruction combines batches 2–4 into one reviewable PR. The merged network baseline permits acyclic C0/C1 driving graphs; later batches must retain transactional publication, stale-state guards, persistence and history. Numerical approximations must state their bounds and unsupported cases without implying full Maya/Softimage parity.

Batch 1 is part of the current baseline. Batches 2–4 are implemented together in the current PR; see `nurbs-refinement-curves-trimming-2026-10-22.md` for contracts, validation and explicit limits. The next proposed batches are 5 (general curved-surface intersections) and 6 (surface blends, fillets and offsets).
