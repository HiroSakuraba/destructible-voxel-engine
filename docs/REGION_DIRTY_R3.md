# R3 — destruction-aware, generation-keyed region work delivery

This batch extends the merged R0/R1 region catalog and R2 runtime streaming planner. It remains behind `DVE_EXPERIMENTAL_REGION_INDEX=ON` and changes no default runtime behavior, saves, packages, rendering API or voxel representation. It does not port the Rust or C# BBTree. The first queue backend uses `std::map`; native SIMD B+ tree comparison belongs to the later R6 benchmarks.

## Architecture and ownership

`RegionDirtyIndex` is authority-thread-only. Its ordered key is `(RegionKey, stable object ID, BrickKey, consumer)`. Consumers are **GPU**, **navigation**, **collision**, and **persistence**; each gets independently acknowledged work. A region is derived using `RegionCatalog::region_of_brick`, which handles negative brick coordinates using signed floor division. Do not share this mutable index between threads; lease tickets may be handed off as owned immutable values, with acknowledgments returned to the authority thread.

### Enqueue and coalesce

Call `submit(worldId, lod, objectId, AppliedBrickEdit, channels)` immediately after an authoritative voxel edit, or `submit_full(...)` for a known full-brick replacement/fill. The index unions changed voxel masks and keeps the latest generation across multiple submissions. An older generation cannot overwrite a newer one. Empty edit masks mean *no work*, not a full update.

`maximumTasks` bounds admission. A multichannel submission is capacity-checked before any task is added. **On `CapacityExceeded`, callers must retain the authoritative event or apply backpressure; never drop an edit.** C++ allocation failures during insertion are still exceptions and need explicit handling in an outer transaction once a production dispatcher is wired.

### Lease, publish, complete

`lease_box(world, lod, inclusiveMinRegion, inclusiveMaxRegion, maximumTickets)` returns deterministic region-ordered tickets. `maximumExamined` is a hard scan cap: exceeding it produces `nullopt` **without issuing partial tickets**. A valid empty vector means no work. This ordered-key query is x-led and filters y/z; it is not a general 3D spatial accelerator.

Each consumer task has at most one active lease. After a successful consumer-side publication, call `complete(ticket,true)`. It retires work only when no edit arrived since the ticket was issued. If a newer edit supersedes the ticket, it returns `Superseded` and keeps all changed bits queued. Call `complete(ticket,false)` on failure to retry. Duplicate or late completion returns `UnknownTicket`.

A replay or checkpoint restore may reduce a brick generation. First call `forget_object(objectId)` on the authority thread to invalidate outstanding tickets, then reenqueue from the restored authoritative snapshot. No previously held ticket may be acknowledged as successful afterward.

### Existing DVE integration

- **GPU**: turn a ticket into `AppliedBrickEdit` with `gpu_edit(ticket)`. Pass it to the existing `GpuBrickMirror::update` and/or `PackedBrickmapScene::update`. These paths already expand the six neighboring brick faces. Do not add six separate GPU tasks for each edit.
- **Navigation**: use `voxel_bounds(brick)` for safe inclusive voxel coordinates and pass the actual meters-per-voxel through the existing `DynamicNavigationWorld::mark_voxel_edit`. Overflow returns `nullopt` rather than invoking signed-overflow UB. Consider nonuniform voxel metric support separately.
- **Collision**: use existing structural/collision rebuild jobs and acknowledge the collision ticket only after required publication.
- **Persistence**: no durable delta log exists in R3. Never acknowledge a persistence ticket until R4/R5 supply a successful atomic save/checkpoint path. Automatic R2 static-unload whitelisting remains off by default for modified content.
- **Frame budgets**: `maximumTickets` is a count, not microseconds or bytes. A later authority-thread dispatcher should consult `WorkDebtGovernor::update(...).patchBrickLimit`, per-consumer queue pressure, frame telemetry, and measured publication budgets.

The R3 batch implements the queue primitive and a real DVE GPU-reference integration test. It **does not yet connect every `VoxelObject` edit producer to a player-wide dispatcher**, so it is not a complete end-to-end game deployment. There are no actual frame-rate improvements established.

## Build and tests

```sh
cmake -S . -B build-region -DCMAKE_BUILD_TYPE=Release \
  -DDVE_EXPERIMENTAL_REGION_INDEX=ON \
  -DDVE_BUILD_TESTS=ON -DDVE_BUILD_BENCHMARK=ON
cmake --build build-region --target \
  dve_region_dirty_tests dve_region_dirty_gpu_tests dve_region_dirty_bench --parallel
ctest --test-dir build-region -R '^dve_region_dirty' --output-on-failure
./build-region/dve_region_dirty_bench
```

The contract tests cover mask union, latest-generation semantics, consumer isolation, duplicate suppression, backpressure, malformed input, negative regions, world/LOD isolation, query-cap rejection, stale/duplicate acknowledgments, retry, retirement, and seeded churn. The **real-DVE** test creates a `VoxelObject`, `GpuBrickMirror`, and `PackedBrickmapScene`, injects an in-flight edit followed by another edit, checks that stale publication is rejected, then validates both representations after the newer edit.

The dedicated `region-index.yml` workflow builds contract tests in GCC Release and Clang sanitizers, and builds the real GPU-linked test in Release. It also fixes malformed existing R0–R2 CTest command quoting so those existing checks execute.

The benchmark uses 100,000 seeded clustered brick edits; it outputs `submit_ns_per_event`, `lease_ns_per_ticket` and coalesced task count as JSON. It measures metadata work only, not GPU uploads, asynchronous scheduling, or game frames.

## Adoption gates

1. Existing R0–R2 tests remain green with and without the experimental flag.
2. R3 contract and full DVE-linked GPU tests compile and pass.
3. Dynamic navigation, collision, save, and GPU work are never acknowledged before publication succeeds.
4. Upstream edits cannot be lost to queue saturation or superseded-ticket acknowledgments.
5. Real DVE frame p95/p99, allocations, and memory growth are measured before switching container implementations; evaluate a SIMD B+ tree only if the `std::map` and hash alternatives lose on representative workloads.

**Scope boundary:** no concurrency claims, persistence guarantees, safe modified-object eviction or C++ bbtree performance claims follow from this PR alone.
