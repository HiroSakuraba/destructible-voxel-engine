# R3.1 — budgeted destruction publication (experimental)

This change connects the R3 region dirty queue to an explicit authority-thread *delivery coordinator*. All code is behind `DVE_EXPERIMENTAL_REGION_INDEX` (default `OFF`), preserving default game behavior. There is no native bbtree dependency.

## What is implemented

- `RegionDirtyDispatcher`: owned copy of incoming `AppliedBrickEdit` batches, bounded admission, retryable queue-full ingress, camera-region interest box, filtered GPU/nav leases, per-object GPU batch update, independent acknowledgment, and telemetry.
- An optional `snapshot_region_game_damage(GameWorld, GameDamageEvent)` producer adapter. Existing `GameWorld::on_damage` supplies the event for both synchronous `damage_sphere` and queued destruction commits. A **full** generation-stamped brick snapshot is used because fragment separation or queued destruction may mutate/replace more than the initial crater bricks; result includes original/fragment IDs and unavailable IDs explicitly.
- A configurable `WorkDebtGovernor` frame target in milliseconds (default remains 8.333 ms for 120 FPS). The dispatcher honors the governor's existing `patchBrickLimit` and an elapsed-time soft budget between batches. A single GPU publication may run longer than that budget; it is not preemptible.
- On GPU deliveries: validate source brick generations before publication, batch by stable object ID, submit to existing `GpuBrickMirror::update` and `PackedBrickmapScene::update`, then retire only matching tickets on success. Failures leave tickets queued. A newer source generation requeues a full-brick update, preventing a stale ticket from clearing fresh work.
- Replacement semantics: `GpuBrickMirror::update` / `PackedBrickmapScene::update` only insert or overwrite slots; they never remove bricks that disappeared from the authority. Snapshots therefore carry `rebuildRequired`, and the host calls `RegionDirtyDispatcher::mark_geometry_replaced(id)` after a queued-destruction commit or fragment split. The next tick rebuilds that object's mirror/packed scene from the authority (at most one explicit rebuild per tick; it is not preemptible and is retained until it succeeds or `forget_object`). A GPU ticket whose brick no longer exists in the authority, or whose generation is older than the ticket's, is reconciled the same way and retired, so it can never retry forever at the head of the queue.
- Ingress forwarding is time-budgeted: at most half of `maximumDispatcherMicroseconds` (checked every 64 edits) is spent moving buffered edits into the queue, the batch cursor is retained, and publication always keeps the remainder of the tick.
- On navigation: call `DynamicNavigationWorld::mark_voxel_edit` for safe bounded voxel coordinates, then acknowledge **invalidation delivery**, NOT successful navigation mesh rebuilding. Collision and persistence tickets are intentionally *never* leased by this dispatcher; they still require explicit downstream publication and durability barriers.
- Strict C++23 contract tests, real `VoxelObject` + actual sphere-damage + both GPU-reference mirrors + nav-invalidation integration test, and a 60/120 FPS governor target regression test.

## Important boundaries

**Not a transparent GameWorld runtime toggle.** The default game runtime has its own damage, physics rebuild, and rendering path. The optional adapter returns an owned snapshot so that a game host can enqueue exactly the committed geometry state without changing GameWorld's callback ABI. Subscribe to `on_damage` only when the host owns a stable lifetime for its listener and dispatcher. If the bridge is detached before GameWorld, ensure the callback does not dereference a destroyed bridge (GameWorld listeners currently have no unsubscribe handle).

The producer's `on_damage` event occurs **after** synchronous fragmentation or the queued destruction commit. This means `report.edits` captured *before* fragmentation is not enough. Full snapshots are deliberately conservative and potentially expensive; profile the number of bricks before optimizing. `destroyed` IDs must be invalidated in the queue and have their renderer resources torn down through the host. Unknown/unavailable object IDs must be handled as error/reconciliation signals, not ignored.

`RegionDirtyDispatcher::offer` returns `Accepted` after retaining a copy, or `Backpressure` (the caller still owns the original event), or `Invalid`. The host must hold and retry *all* nonaccepted snapshots. It must call `forget_object` for identity changes/restores, but **must preserve pending unsaved persistence state** before any such reset. For GameWorld listeners, prefer a dedicated GPU/nav-only region queue; never use the same `forget_object` operation to discard pending durable persistence tasks. This adapter is explicitly for **derived-data** delivery; it does not implement persistent destruction.

R3.1 uses `std::map` as the ordered queue backend. It does not solve spatial indexing, GPU sparse-brick addressing, authoritative restoration, or durable eviction. Per-frame `patchBrickLimit` limits tickets, and `maximumDispatcherMicroseconds` is an *observed soft cutoff between object batches*; it is not a hard execution deadline. The `region_dirty` queue's `maximumExamined` rejects unbounded x-first range queries. An empty/invalid target does not acknowledge unfinished GPU or nav work.

## Example: producer and dispatcher lifecycle

```cpp
// Opt-in. Owned by the game host, with a lifetime shorter than GameWorld.
// Register only GPU + navigation in this first pass (not persistence/collision).
RegionCatalog catalog;
RegionDirtyIndex queue(catalog);
RegionDirtyDispatcher dispatcher(queue);
std::deque<RegionGameDamageSnapshot> committed;
world.on_damage([&](const GameDamageEvent& event) {
    committed.push_back(snapshot_region_game_damage(world, event));
});

// On the authority tick: retry snapshots before checking the queue. Do not
// discard snapshots that fail admission or contain unavailable IDs.
// For each snapshot: call dispatcher.forget_object(id) only when that object
// was destroyed or its generations were intentionally reset; offer each
// refreshed object's fullEdits using GPU | Navigation channels, and call
// dispatcher.mark_geometry_replaced(id) when refreshed.rebuildRequired.
// Keep unaccepted snapshots in `committed` until offer returns Accepted.

WorkDebtGovernor governor(240, 1000.0 / 60.0);
const WorkBudget budget = governor.update(frameTelemetry);
const RegionDirtyTargetLookup targets = [&](std::uint64_t id) -> RegionDirtyTargets {
    // Return current generation-validated references to the authoritative
    // VoxelObject, plus that object's mirror, packed buffer, navmesh, voxel metric.
    return lookup_region_publication_targets(id);
};
const RegionDirtyDispatchStats stats = dispatcher.tick(cameraRegion, budget, targets);
```

The example is deliberately schematic: `lookup_region_publication_targets` belongs to the host's object/renderer ownership, and the host must reconcile queued and destroyed objects before offering. R3.1 does not register a hidden global callback or mutate GameWorld's save format.

## Testing and adoption gate

Build with `DVE_EXPERIMENTAL_REGION_INDEX=ON` and use the dedicated `region-index.yml` CI, or:

```sh
cmake -S . -B build-region -DCMAKE_BUILD_TYPE=Release \
  -DDVE_EXPERIMENTAL_REGION_INDEX=ON -DDVE_BUILD_TESTS=ON
cmake --build build-region --target \
  dve_region_dirty_dispatch_tests dve_region_dirty_dispatch_real_tests \
  dve_region_dirty_dispatch_budget_tests --parallel 2
ctest --test-dir build-region -R '^dve_region_dirty_dispatch' --output-on-failure
```

Target properties: `dve_region_dirty_dispatch` links `dve_region_dirty_index` and `dve::core` only when the experiment is enabled; `dve_region_game_damage` adds a narrow opt-in `GameWorld` snapshot adapter.

## Next required production work

1. Host-owned subscription and lifecycle reconciliation (no implicit raw-pointer callback lifetime).
2. Durable, budgeted collision/persistence transaction interface; never retire those consumers just because their work was scheduled.
3. Streaming and navigation/physics integration tests under large world-destruction scenarios, profiling p95/p99 latency and memory.
4. Benchmark `std::map` versus a C++ SIMD bbtree candidate on the same workload before changing containers.
