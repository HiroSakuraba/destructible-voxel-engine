# Editor spatial performance — 2026-10-06

This follow-up targets the work triggered when bounds or selection diagnostics
must rebuild after an edit. It reduces allocations and pointer chasing without
adding a revision cache: direct voxel mutations are still observed immediately.

## Changes

- Tight occupied bounds use eight occupancy words per brick instead of visiting
  every occupied voxel. Transforming the resulting eight corners is unchanged.
- Diagnostics derive surface voxels from the existing six bitwise face masks.
  Connectivity uses one visited bitset and six neighbor indices per brick,
  plus a contiguous work vector reused between components. This replaces two
  per-voxel ordered sets and a fresh deque for each component.
- Collision validation uses row masks and one coverage bitset per stored brick.
  It still rejects air, overlapping boxes, degenerate boxes, and missing voxels,
  and accepts boxes crossing brick boundaries, including negative coordinates.
- Proxy construction appends directly into the final vector and finds the first
  remaining voxel with a word scan rather than enumerating all remaining bits.
  The generated boxes and their order are unchanged.

For a 64-brick object, validation's coverage payload is 4 KiB in one allocation,
replacing one map allocation per occupied voxel. Connectivity metadata is 88
bytes per stored brick, plus the work vector. Empty brick headers also consume
scratch space; extremely sparse or mostly deleted scenes therefore have smaller
benefits. The existing voxel/mass-point vectors remain, preserving numerical
summation order and results.

## Measurements

GCC 13.3, CMake Release (`-O3 -DNDEBUG`), same machine and configuration before
and after, single-threaded warmed runs. The baseline executable was saved before
changing the spatial implementation. Times are medians: 100 iterations for
bounds and eight for validation/diagnostics. Fixtures and ordinary `operator new`
allocation counters are in `apps/editor_spatial_bench.cpp`.

| Fixture | Occupied voxels | Diagnostics before | Diagnostics after | Allocations before → after |
| --- | ---: | ---: | ---: | ---: |
| Solid, 64 bricks | 32,768 | 29.801 ms | 0.789 ms | 99,163 → 29 |
| Damaged, 64 bricks | 24,487 | 22.931 ms | 1.381 ms | 74,587 → 36 |
| Sparse, 512 bricks | 512 | 0.226 ms | 0.154 ms | 3,086 → 18 |
| Checkerboard, 8 bricks | 2,048 | 1.126 ms | 0.130 ms | 10,320 → 20 |

| Fixture | Tight bounds before → after | Validation before → after | Validation allocations before → after |
| --- | ---: | ---: | ---: |
| Solid | 58.278 → 1.262 µs | 7,603.600 → 4.056 µs | 32,768 → 1 |
| Damaged | 43.395 → 1.532 µs | 6,195.940 → 170.877 µs | 24,487 → 1 |
| Sparse | 3.886 → 3.405 µs | 43.986 → 12.569 µs | 512 → 1 |
| Checkerboard | 3.826 → 0.340 µs | 295.315 → 44.326 µs | 2,048 → 1 |

All benchmark diagnostic counts and checksums matched. These are CPU
microbenchmarks, not measured menu, frame, or GPU latency. Allocation counters
include returned data but exclude aligned allocation APIs, allocator overhead,
and peak resident memory. No hardware cache-hit counters were collected.

## Validation

Ten local test executables passed: core, editor spatial, editor, render cache,
menu command center, settings, shortcuts, SDL host, desktop editor contract, and
AI assistant.

The new spatial suite compares bitset bounds with voxel enumeration, all single
bits and 500 random masks; checks connectivity/surface results with an independent
set-based oracle; and checks exact box coverage with another oracle. Fixtures
cover empty retained bricks, solid and fragmented shapes, randomized materials
and anchors, collision disabled, transformed bounds, negative/cross-brick boxes,
invalid coverage, and coordinate-limit clipping.

A separate comparison compiled the original diagnostics and proxy code alongside
the new version. All diagnostic fields (including mass, centers of mass, inertia,
materials, and warnings) and generated box order matched across 31 fixtures.
AddressSanitizer and UndefinedBehaviorSanitizer passed on the spatial tests and
changed implementation sources. LeakSanitizer was disabled because this runtime
reports that it cannot operate under ptrace; unaffected libraries used the
existing Release build.

Build and run with an editor-enabled configuration:

```sh
cmake --build build --target dve_editor_spatial_bench dve_editor_spatial_tests
ctest --test-dir build -R '^dve_editor_spatial_tests$' --output-on-failure
./build/dve_editor_spatial_bench
```
