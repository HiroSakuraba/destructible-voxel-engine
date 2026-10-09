# Shared-brick fragmentation

Fragment plans built from the same connectivity snapshot are validated and committed as a
batch. If two detached components occupy one source brick, their removal masks are combined
before the source brick is edited. A stale or overlapping plan rejects the whole batch before
changing source voxels. The single-plan entry point delegates to the batch implementation.

The two-prong GameWorld regression failed on `main` (one fragment instead of two). The new
connectivity test checks one generation bump and transactional stale-plan rejection. The
connectivity, GameWorld, and save tests passed with this change stacked on the collision
rebuild fix.
