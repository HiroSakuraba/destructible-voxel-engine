# R2 — region-driven deferred residency (stacked on R0/R1)

**Scope:** optional C++23 `RegionStreamingPlanner` and `RegionStreamingRuntime`, behind `DVE_EXPERIMENTAL_REGION_INDEX=ON`. This is an experimental coordinator using DVE's existing `RuntimeSceneWorld` authority thread, `RuntimeSceneStagingExecutor`, and `RuntimeDeferredObjectStaging`. It is not a replacement for `FlatBrickMap`, `PackedBrickmapScene`, the renderer, physics, or `DVESAVE1`.

## API and responsibilities

- Register stable scene object IDs in `RegionCatalog` with `set_object_regions`/`set_object_bounds`. A deferred object's asset bounds are **not generally available** until decoding, so the scene builder/importer must supply a conservative bounds estimate or explicit region memberships. Objects with no catalog membership are never scheduled or evicted.
- `RegionStreamingPlanner::camera_region(catalog,world,lod,{x,y,z},metresPerVoxel)` maps metres to signed brick and region coordinates. Call it on the authority thread using actual camera position (world metres, not voxel indices). Invalid/nonfinite/out-of-range coordinates return `nullopt`.
- The planner queries two Chebyshev-radius boxes: inner `loadRadius` for load demand, outer `retainRadius` for eviction hysteresis. Both use `RegionCatalog::query`; if either exceeds `maximumExamined`, planning returns `nullopt` and **no eviction or new publication** occurs in that tick. The x-first bounding range is not an exact 3D broad-phase; workloads with large x spans may exhaust the query budget.
- An object's membership may span several regions. Stable object IDs are deduplicated. Load candidates are ordered by distance to their closest indexed region and then ID, independent of hash-map iteration order. In-flight candidates are excluded from re-issuance. `loadsPerTick`, `unloadsPerTick` are request limits, not byte or microsecond budgets.
- The runtime coordinator snapshots `RuntimeScene::objects()` on the authority thread. It queues up to `maximumInFlight` deferred jobs on the existing bounded `RuntimeSceneStagingExecutor`. The executor performs asset I/O and derived-data preparation. A tick polls futures with `wait_for(0)` and publishes at most `maximumPublicationsPerTick` **completed jobs**, again on the authority thread via `publish_staged_deferred_object`.
- If a camera moves away while staging runs, request cancellation. A completed job is published only if its object remains in the new **inner** load radius, remains deferred, and its underlying residency generation is accepted by DVE. Otherwise staging is discarded. `RuntimeSceneWorld` enforces staging generation and publication preconditions.
- `retryDelayTicks` prevents a failed load from being resubmitted every frame. Pins prevent eviction; a caller may pin mission-critical objects, active physics or edited objects.

## Safety / scope boundary

**Automatic unloading is OFF by default**, even for anchored static objects. `enableStaticUnloads=true` *and* `allow_static_unload(id,true)` are required for an object to be eligible, and dynamic objects are always excluded. The caller must only whitelist static objects whose state is reproducible or whose edits have been safely persisted. R4 will provide a stronger modified-state/persistence contract. Do not turn on unloading globally in a game until that exists.

The coordinator has no ownership of a scene or executor. The caller must keep `RuntimeSceneWorld` and `RuntimeSceneStagingExecutor` alive until pending work completes or their cancellation is drained; do not destroy the scene or executor out from under active tasks. It is also the caller's responsibility to keep catalog and scene IDs consistent across scene unloading, hot-reload and checkpoint restoration. An absent scene cancels pending work, and does not act on unloaded object IDs. The underlying scene publication rejects stale staged objects.

**Still missing for a game demo:** automatic manifest-bound region membership import; frame integration with the camera; per-object bytes and global residency caps; dynamic-object state migration and safe eviction; failure retries with user diagnostics; network replication and cross-level world identifiers; spatial reindexing for moving objects; and production-grade multi-scene/scene-hot-reload validation (the local headless runtime contract uses a stub; an additional real house-scene fixture test runs in CI). This PR supplies the independently testable planner and a compiled bridge to DVE's real deferred API, not a fully wired streaming game.

## Sample integration (illustrative, after creating a streamable scene)

```cpp
// options.loadAllObjects = false; // reserve stable IDs, defer payloads
// load the scene once and populate catalog from known world-space object bounds
// RuntimeSceneWorld world(&physics, &gpu);
// RuntimeSceneStagingExecutor executor({ ... }); // retain for whole session
// auto scene = world.load_scene_package(path, options);
// RegionStreamingRuntimeConfig config;
// config.planner = { .loadRadius = 2, .retainRadius = 4, .maximumExamined = 200000,
//                    .loadsPerTick = 2, .unloadsPerTick = 1, .minimumResidentTicks = 120 };
// RegionStreamingRuntime streamer(catalog, world, executor, scene.handle, config);
// auto key = RegionStreamingPlanner::camera_region(catalog, worldId, 0,
//     {camera.x, camera.y, camera.z}, metresPerVoxel);
// if (key) { const auto stats = streamer.tick(*key, worldTick); /* publish telemetry */ }
```

Do not block on `std::future::get` before testing readiness. Never publish staged data from a worker thread. Do not use a region index to provide GPU brick data: keep the existing packed-brickmap and GPU mirror paths.

## Tests

| Case | Expected |
|---|---|
| Negative metres and coordinates, first region boundary | Correct signed floor division |
| Overlapping object membership, duplicate regions | One job per stable object ID |
| World ID / LOD isolation | Other worlds do not stream |
| Planner under query examination cap | No partial range / no eviction |
| Pinning and minimum-resident ticks | No premature eviction |
| Dynamic object outside outer radius | Cannot be unloaded |
| Camera moves while a stage is in flight | Cancel request; completed obsolete stage discarded |
| Two jobs become ready in one frame, publish cap 1 | One publication per tick, second retained |
| Stage retry after failure | No hot-loop re-queues before retry tick |

A stubbed executor contract verifies the coordinator's state machine without filesystem or GPU dependencies. The optional DVE CMake target compiles the bridge against the **real** `runtime_scene.hpp`. `dve_region_streaming_real_tests` also reserves DVE's existing multi-object house with its assets deferred, loads two objects using actual staging/publish/render/physics APIs, verifies readback and resource teardown. That test requires the full repository checkout; it was **not executed in the isolated local harness**. More failure-injection/rollback integration fixtures remain. Existing save and GPU readback tests must remain unchanged.

### Build

```sh
cmake -S . -B build-region -DCMAKE_BUILD_TYPE=Release \
  -DDVE_EXPERIMENTAL_REGION_INDEX=ON -DDVE_BUILD_TESTS=ON
cmake --build build-region --target dve_region_streaming_tests dve_region_streaming_runtime_contract_tests dve_region_streaming_real_tests --parallel
ctest --test-dir build-region -R 'dve_region_(catalog|streaming)' --output-on-failure
```

`DVE_EXPERIMENTAL_REGION_INDEX=OFF` compiles no R1/R2 code. This stacked PR targets `feature/region-index-r0-r1`; merge R0/R1 first, then rebase/retarget R2 onto `main`.

