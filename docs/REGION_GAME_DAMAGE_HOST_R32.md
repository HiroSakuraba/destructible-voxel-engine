# R3.2 — GameWorld damage subscription and bounded derived-state ingestion

**Opt-in:** `DVE_EXPERIMENTAL_REGION_INDEX=ON`; default OFF. This PR adds a host-owned `RegionGameDamageHost` that consumes committed damage events without modifying `GameWorld` or its save format. It uses the R3.1 `RegionDirtyDispatcher` and **a dedicated GPU/navigation-only queue**.

## Mechanics

1. The constructor registers `GameWorld::on_damage` and `GameWorld::on_destroyed` listeners capturing only a `std::weak_ptr` to its inbox. Since GameWorld currently has no listener unsubscribe token, both callbacks become inert when the host is destroyed (they stay registered, so do not create hosts per frame); they do not capture `this` or a GameWorld pointer. `on_destroyed` is required because `retire_oldest_debris()` and pool release destroy objects without any damage event. A listener invoked off the constructing thread records nothing unsafely; it only bumps an atomic counter, which makes the next `tick()` do a full rescan (`foreignThreadEvents`).
2. The listener records the primary object's ID, destroyed IDs, and spawned fragment IDs. It does not retain borrowed VoxelObject pointers or pre-fragmentation edit reports.
3. On the host authority thread, `tick()` reconciles removals, samples current authoritative brick keys/generations, and offers `Bitset512::full()` updates to the dispatcher in chunks. An object may require multiple ticks. If damaged again mid-transfer, the obsolete snapshot is discarded and replaced.
4. Ordinary listener storage is capped by `maximumPendingIds`. Overflow triggers a full authoritative rescan, which may cost O(all world objects) and allocate more than the normal cap (the rescan sweep is not bounded by `maximumPendingIds` and is served round-robin after fresh damage events, so a world with more objects than the cap still converges); an event is never silently dropped.
5. Each tick then calls the R3.1 dispatcher with a camera region, WorkDebtGovernor-supplied `WorkBudget`, and current object target lookup. The host enforces `maximumOffersPerTick`, `maximumSnapshotsPerTick`, and a per-object snapshot cap. An object above `maximumSnapshotBricks` is moved to a retained *oversized* set (`oversizedObjects`, `snapshotLimit`) and skipped so later ids still progress; it is retried only when damaged again or on a full rescan. Every snapshot calls `forget_object()` then `RegionDirtyDispatcher::mark_geometry_replaced()` (R3.1 follow-up), because the snapshot only names bricks that still exist and incremental GPU updates never drop slots; the dispatcher performs at most one authority rebuild per tick.

## Important limits

This is **derived-state delivery only**, not persistence, collision completion or a journal. `forget_object()` clears queue tickets, so the queue MUST NOT include unacknowledged persistence/collision obligations. Dynamic debris is not evicted by this feature.

Geometry that removes whole bricks is handled by the per-snapshot `mark_geometry_replaced()` request. This costs one full mirror/packed rebuild per damaged object (conservative: the host keeps no previous brick set to prove nothing disappeared). Collision and persistence still need their own durable path.

The callback and `tick()` assume GameWorld's authority-thread model. They are not thread-safe concurrent event ingestion. The caller must own and outlive the host's `GameWorld` and `RegionDirtyDispatcher`. Target lookup must return stable references throughout one dispatcher call.

## Tests and build

```sh
cmake -S . -B build-region -DCMAKE_BUILD_TYPE=Release -DDVE_EXPERIMENTAL_REGION_INDEX=ON -DDVE_BUILD_TESTS=ON
cmake --build build-region --target dve_region_game_damage_host_contract_tests dve_region_game_damage_host_real_tests --parallel 2
ctest --test-dir build-region -R '^dve_region_game_damage_host_' --output-on-failure
```

Contract tests compile the actual host implementation against minimal fake GameWorld/dispatcher interfaces and check: mid-chunk re-damage, event inbox overflow and full-scene rescan convergence, damage-less destruction (debris retirement), oversized-object skipping, geometry-replacement requests, wrong-thread rejection, and weak-listener cleanup. Real-DVE tests run `damage_sphere` on a GameWorld object, verify a GPU update reaches both mirrors, and confirm teardown. Local contract builds passed under strict warnings and AddressSanitizer/UndefinedBehaviorSanitizer. Full-DVE CMake/CI results must be verified in GitHub.

No benchmark or frame-time speedup is claimed. Next: integrated topology-replacement/rebuild work, then durable state-change journaling and safe eviction.
