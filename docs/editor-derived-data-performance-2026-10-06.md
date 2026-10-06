# Editor derived-data cache and draw-list sort

Hover picking, the selection box and the inspector's voxel count each re-walked
every brick of an object to recompute bounds or totals that only change when the
object is edited. This change caches those derived values on `VoxelObject`,
keyed by the content revision PR #45 added, hoists two per-voxel set lookups
out of the draw-list emit loop, and sorts mid-sized draw lists by depth with a
radix pass over 8-byte proxies instead of a comparison sort over 48-byte
items. Baseline: main commit `de51c33` (after PRs #42-#45).

## Behavior and compatibility

- `VoxelObject` gains `brick_extent_bounds()` and `occupied_bounds()`
  (`DerivedBounds`: minimum, maximum, valid) and serves
  `occupied_voxel_count()` from the same cache. One walk over the brick map
  computes all three; the walk repeats only when `revision()` has changed
  since the last query, so repeated queries between edits are O(1). The
  revision rules are unchanged: every brick mutation path already calls
  `touch()`, the counter is process-wide and never 0, so a cached value can
  never be mistaken for current after an edit.
- The semantics are the ones the previous per-call walks had, exactly. Extent
  bounds cover whole bricks (maximum is the far brick corner, minimum the
  nearest brick origin); a brick whose voxels were all removed still counts
  toward the extent. Occupied bounds are the tight box around occupied voxels
  with an inclusive maximum, and count only bricks that contain a voxel.
  `valid` is false when no brick (extent) or no voxel (occupied) qualifies,
  and the count is 0 for an empty object.
- Moves stay consistent: move construction leaves the new object's cache
  stale (it recomputes on first use), the moved-from object gets a fresh
  revision as before and so reports invalid bounds and a zero count, and move
  assignment swaps the cache together with the content it describes.
- `local_voxel_bounds` and `object_world_bounds` in the editor viewport now
  read the cache. Their callers include hover picking (once per mouse move),
  the renderer's per-frame selection box, diagnostics, the splat-coverage
  check inside every draw-list rebuild, and the text3d draw path.
- The emit loop of `build_voxel_draw_list` hoists the per-object selection
  test out of the per-voxel lambda and serves each item's anchor flag from
  the per-brick anchor bitsets the culling path already builds (now built
  whenever an object has anchors), replacing a `std::set` lookup for the
  selection and a `std::set<Int3>` lookup for the anchor on every emitted
  voxel.
- The draw order is unchanged: depth descending, then object id, then voxel.
  That order is total (each voxel is emitted once per object, so no two items
  compare equal). For lists of 8,192 to 65,536 items the sort radix-sorts
  `{depth key, original index}` proxies by depth (4 stable byte passes over
  the order-preserving float key, with -0.0 canonicalized to +0.0 so the two
  zeroes tie as float equality ties them), applies the permutation to the
  items once, and repairs each maximal run of equal depths with the total
  comparator. Outside that window the previous `std::stable_sort` runs
  unchanged. Depths reaching the list are always finite (`project_with`
  rejects NaN and out-of-range depths), which the key mapping relies on.

## Measurements

Two-core VM of the same class as the earlier editor performance notes,
medians, baseline binary and changed binary measured back to back under the
same conditions.

| Workload | Before | After |
| --- | --- | --- |
| Hover pick, 64 objects x 64^3 cubes, miss | 119.0 us | 6.6 us |
| Hover pick, same scene, hit | 119.7 us | 8.8 us |
| `object_world_bounds`, all 64 objects | 1,064 us | ~16-21 us |
| Draw-list rebuild, 64^3 cube, culling on (46,144 items) | 8.33 ms | 6.4-6.6 ms |
| Draw-list rebuild, same, culling off (120,000 items) | 20.8 ms | ~21 ms (unchanged) |

Almost all of the old hover-pick cost was the bounds walk, not the voxel
raycast: hit and miss cost the same before, and the remaining ~7-9 us is the
per-object AABB tests plus the raycast itself. The rebuild's emit/sort split
(measured with phase timers) was roughly 55/45 before; the culled rebuild's
gain comes from the sort window below and from the splat-coverage check no
longer walking bricks for bounds.

## What was tried and not kept

- Radix-sorting the 48-byte items directly was slower than the comparison
  sort at 120k items (25.4 ms vs 20.8 ms whole rebuild): the scatter ranges
  over ~6 MB per array and falls out of cache. Sorting 8-byte proxies and
  permuting once fixed the 46k case (above) but the permutation gather at
  120k items still ranges over ~12 MB of item arrays and loses to the
  comparison sort, which is why the radix path is capped at 65,536 items
  instead of being used for every large list. The window's upper edge is the
  measured cross-over region on this machine, chosen conservatively.
- A brick-skipping DDA for `raycast_voxels` was considered and not attempted:
  the current walk's exact edge-hit ownership semantics would need a
  differential oracle before any change is safe, and picking is no longer
  bound by it after this change.

## Bug hunt

The full suite (264 tests) was also run under ASan + UBSan with leak
detection. No sanitizer finding points at engine code; the draw-culling tests
(including the new ones below) pass under the sanitizers. The failures under
the sanitizer build are configuration artifacts: the packaging tests reject
the bundled `libasan`/`libubsan` runtimes in the sanitizer install tree (the
same tests pass on the Release tree), leak detection reports 320 bytes in
system libfontconfig's global cache, and `dve_editor_synth_tests` overflows
the default 8 MB stack constructing the editor controller under ASan's stack
instrumentation (it passes with a larger stack; the controller's by-value
footprint is large and worth watching, but no invalid access occurs).
`dve_cpack_test` fails on any machine without SDL3 installed, including this
one, because the desktop editor is disabled at configure time while the test
expects its binary in the package; it would be more robust for the test to
skip in that configuration.

## Verification

- New tests in `dve_editor_draw_culling_tests`: cached bounds and count
  against independent brute-force brick walks over randomized objects
  (negative coordinates, emptied bricks), again after edits made with a warm
  cache (growing, shrinking, a mutable `find_brick`), after move construction
  and move assignment; world bounds equal the tight occupied box scaled; and
  the draw list is item-for-item identical (every field, bit-exact floats) to
  the reference comparison sort on adversarial scenes, including an
  orthographic straight-down view of flat slabs where whole z layers share
  one depth, in both culling modes. The existing pixel-identity culling suite
  is unaffected.
- Release suite on this branch: 261/264 pass. The failures are the two
  sandbox network tests and `dve_cpack_test` (no SDL3 on this machine, see
  above). `dve_tests`, `dve_install_tree_test` and the packaging checks pass.
