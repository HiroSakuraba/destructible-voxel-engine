# R0 / R1: experimental region metadata catalog

Status: **opt-in library and headless probes only**. No game runtime, `.dvepak`, GPU structure,
voxel object, save format or streaming behavior is replaced. The implementation is NOT the
Rust bbtree algorithm; it compares cheap C++ ordered-index policies first.

## Building in DVE

```sh
cmake -S . -B build-region -DCMAKE_BUILD_TYPE=Release \
  -DDVE_EXPERIMENTAL_REGION_INDEX=ON -DDVE_BUILD_TESTS=ON -DDVE_BUILD_BENCHMARK=ON
cmake --build build-region --target dve_region_catalog_tests dve_region_bench -j
ctest --test-dir build-region -R dve_region_catalog_tests --output-on-failure
./build-region/dve_region_bench
./build-region/dve_region_bench --large
```

`DVE_EXPERIMENTAL_REGION_INDEX` defaults to OFF. When OFF, no new targets are built and
there is no runtime behavior change. The `dve_region_bench` target is created only if
`DVE_BUILD_BENCHMARK=ON`; it links existing `dve::core` to capture REAL `VoxelObject`,
`GpuBrickMirror` and `PackedBrickmapScene` baselines. Benchmarks print JSON Lines; record
compiler, git SHA, CPU, OS, build flags and cache state alongside results. This PR does
not claim a speedup, and times from different machines must not be compared naively.

## R0 traces

The headless benchmark records: brick fill, `VoxelObject::state_hash`, const brick find,
full GPU mirror build, packed brickmap build, one authoritative brick edit, both incremental
GPU update paths, published upload bytes, and validation of both mirror representations.
It also times construction, exact lookups and small ordered windows for both region
catalog policies. Running this harness is not evidence of a full engine frame improvement:
there are no graphics driver timings, file I/O, navigation rebuild or camera flight.

## R1 contracts

- World ID and LOD are part of every key; the other fields are signed integer **region** coordinates.
- `bricksPerRegionAxis` defaults to 16; one DVE brick contains 8x8x8 voxels. Coordinates
  are in BRICKS, never implicitly metres.
- `region_of_brick` uses mathematical floor division, handling negative values including
  `INT32_MIN`, with no UB when width is positive.
- Local half-open 3D bounds can be transformed with an explicit affine matrix and metres
  per voxel. The resulting conservative world-space AABB is quantized to inclusive brick
  coordinates. A transformed upper endpoint stays exclusive unless every non-zero
  coefficient of that world axis is negative (a reflected axis), in which case the local
  minimum face it comes from is included and the endpoint is inclusive (it may land in the
  next brick). This is intended for static object membership; fast-moving bodies and
  physics ownership still require a separate residency/pinning policy.
- Object memberships are many-to-many: ordered unique stable `uint64_t` object IDs per
  region and reverse object-to-region membership. No live object or voxel pointers.
- Replacing memberships removes stale forward entries; deleting an object removes all
  its memberships. Duplicate keys and no-op updates do not bump the revision.
- `HashSorted` stores hash-indexed region vectors and lazily caches a sorted view;
  `OrderedMap` uses `std::map` for range iteration. Tests cross-check them, rather than
  assuming an ordered tree is faster.
- `query()` is a lexicographic x-range seek plus y/z filtering (NOT a general 3D spatial
  index). Large scans fail explicitly at the supplied `maximumExamined` bound; no
  accidental partial result escapes.
- `keys_for_brick_bounds()` rejects an oversized Cartesian product before allocation,
  using 64-bit loop and count arithmetic to avoid signed overflow.
- Catalog writers and mutable read APIs are **single-threaded**. Copies from `entries()`
  are detached and may be published as immutable snapshots by a later planner; no unsafe
  optimistic reads have been added. R2 handles streaming, publisher epochs, hysteresis,
  dynamic pins and memory-pressure work scheduling.

## Review and adoption gates

1. Run on main and PR with same compiler and seed. No `FlatBrickMap` replacement.
2. Compare `HashSorted` with `OrderedMap` at 12k and 50k objects and under churn.
3. Note first-sort cost, cache invalidations, query candidate count, and peak RSS.
4. Confirm exact `VoxelObject` state hash and both GPU mirror validations before and
   after changes; future GPU/native tests must compare actual rendering and publication.
5. Roll back by setting `DVE_EXPERIMENTAL_REGION_INDEX=OFF`; R2 should remain blocked
   until numbers and the region catalog contracts have been reviewed.

## Limitations & caveats

No automatic integration into `RuntimeSceneWorld` yet, no multiplayer authority or save
serialization changes, no page-based B+ tree, no real asynchronous streaming. Regions
are spatial discovery units, not substitutes for voxel `FlatBrickMap` or collision BVHs.
Membership insertion rolls back additions on allocation exceptions under its single-writer
contract. New stream/planner consumers should still treat failed allocations as an
operation failure and must not publish incomplete world state.