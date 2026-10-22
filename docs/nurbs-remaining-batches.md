# Remaining NURBS batches

Requested sequence:

1. Network transforms and reuse: whole-component placement and grouping, complete-network duplicate/copy with endpoint ID remapping, prefab capture/instances and atomic history.
2. Network refinement: user-directed knot insertion and degree elevation propagated across incident seams while preserving shape and continuity.
3. Persistent curves on surfaces: editable UV curves, retained projection relationships and updates after surface editing.
4. Arbitrary trimming: oriented closed loops, holes, selected regions, trim-aware tessellation, untrim and undo.
5. General surface intersections: bounded numerical tracing of curved branches, boundary/tangency diagnostics and validated trimming curves.
6. Surface blends, fillets and offsets: transition strips, tangent/curvature controls, rolling-ball fillets and trimming support.

Implement and validate these in order, with a separate reviewable PR and HTML worklist update for each. The merged network baseline permits acyclic C0/C1 driving graphs; later batches must retain transactional publication, stale-state guards, persistence and history. Numerical approximations must state their bounds and unsupported cases without implying full Maya/Softimage parity.

Batch 1 is implemented and locally validated, ready for review. Next: batch 2. Batches 2–6 remain authorized and pending; continue in the order above.
