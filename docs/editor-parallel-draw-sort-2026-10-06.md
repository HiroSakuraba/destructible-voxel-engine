# Parallel depth sort for the voxel draw list

Orbiting the camera rebuilds the voxel draw list every frame. Measured on a scene of
sixteen solid 64^3 objects (8,192 bricks, the list capped at 120,000 items), a rebuild took
about 20 ms, and about 17 ms of that was the far-to-near sort: lists over 65,536 items use
the comparison sort (#52's radix path stops there because of a cache cliff). Projecting the
voxels is only about 3 ms. So this change parallelizes the sort, not the projection.

## What changed

- `build_voxel_draw_list` gains an overload that takes a `JobSystem*`. Lists of 32,768
  items or more are cut into one run per thread; each run is sorted at the same time (with
  the existing single-threaded sort, so radix or comparison as before), then pairs of runs
  are merged level by level, each level in parallel.
- The draw order is a total order (depth, then object id, then voxel; no two items tie),
  so the result is item-for-item the single-threaded sort's.
- `EditorVoxelDrawListCache` uses one worker pool shared by every cache in the process,
  created on first use. Only one thread uses it at a time; a second caller (for example a
  controller on another thread) does not wait, it sorts on its own thread.
- An early version split the projection across threads instead. It was slower here: with
  the cap, the serial loop stops early while parallel batches project past it, and the
  projection was never the expensive part.

## Measurements

Release `-O2`, the scene above, 1280x720 viewport, 15 orbit poses, medians. This machine
has 2 cores, so the pool has one worker plus the calling thread.

| Culling | Single-threaded rebuild | With the pool (2 threads) |
|---|---:|---:|
| on | 20.2-20.3 ms | 14.7-15.1 ms |
| off | 18.6-20.3 ms | 13.7-15.3 ms |

With more cores the sort runs shrink further but the final merge (one pass over the
list) and the 3 ms projection stay; that was not measured here.

## Validation

- `dve_editor_draw_culling_tests` adds a comparison of single-threaded and pooled builds
  (three worker threads) over 24 camera poses, culling on and off, and caps of 120,000,
  40,000 and 4,321 items: all 144 lists are identical field for field (bitwise for the
  floats); 96 of them were large enough to take the parallel sort.

Full build (Release, SDL3 unavailable): 252 of 253 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
