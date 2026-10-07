# Thread-safe VoxelObject derived-data cache

PR #52 cached `VoxelObject::brick_extent_bounds()`, `occupied_bounds()` and
`occupied_voxel_count()` against the content revision. The cache lives in `mutable`
members that the first query after an edit fills in, so two threads querying the same
object at once both wrote it: a data race on a `const` call. Before #52 those queries only
read the bricks, and `const` access to a `VoxelObject` was safe from several threads.
Nothing in the engine queries one object from two threads today, but shared read-only
assets and worker pools make it an easy trap.

## The fix

- `ensure_derived()` loads the cached revision with acquire ordering; if it matches the
  object's revision the cache is ready and the query just reads it.
- Otherwise it takes `derivedMutex_`, checks again, refreshes, and publishes by storing the
  revision with release ordering, so a reader that sees the new revision also sees the
  values written before it.
- Move assignment marks the cache stale instead of swapping the cache fields (the mutex and
  atomic are not movable; the moved-in content is re-derived on the next query, as before).

The ready path is one atomic load (a plain load on x86), so the per-query cost #52 measured
is unchanged. Edits still must not run concurrently with queries, as for any other member.

## Validation

- `dve_editor_draw_culling_tests` adds a concurrent-reads case: after each of 20 edits, four
  threads query all three values 200 times each and must all see the correct result.
- Under ThreadSanitizer, the same access pattern reports a data race on every run with the
  #52 code (3 of 3 runs) and none with this change (3 of 3).

Full build (Release, SDL3 unavailable): 252 of 253 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
