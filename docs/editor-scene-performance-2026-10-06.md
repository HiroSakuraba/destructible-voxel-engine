# Editor scene fingerprint and draw-list culling

This change makes the per-frame "has the scene changed?" check independent of brick
count, and cuts the cost of rebuilding the voxel draw list after a camera move by
skipping voxels that cannot be seen and by hoisting per-view projection math out of
the per-voxel loop. Baseline: main commit `3232fcf` (PR #43).

## Behavior and compatibility

- `VoxelObject::revision()` is a content revision drawn from a process-wide atomic
  counter, so values are never shared between objects or states, even at a reused
  address. It changes on `set_voxel` and `apply` when they insert a brick or change a
  voxel, on every `fill_brick` and `replace_brick`, on every call of the mutable
  `find_brick` overload, and on move assignment. A moved-from object receives a fresh
  revision. Edits made later through a held `Brick*` are not observed; `apply()` is
  the supported mutation path. Read-only callers in `runtime_scene.cpp`,
  `connectivity.cpp`, `damage.cpp` and `game_world.cpp` that bound to the mutable
  overload now use the const one, so reads do not churn the revision.
- `editor_scene_render_fingerprint` hashes each object's voxel revision and brick
  count instead of every brick key, generation and occupancy count. Its cost is now
  O(objects + anchors) rather than O(bricks). The hasher mixes one 64-bit word per
  multiply instead of eight byte-wise FNV steps. Fingerprints are compared only within
  a process and are never persisted, so their values changing is not a format change.
- `build_voxel_draw_list` skips voxels whose whole 5x5x5 neighbourhood is solid
  (`EditorViewportSettings::cullEnclosedVoxels`, default on). The exposed set is
  computed per brick with a separable bitwise erosion over the 26 neighbouring bricks,
  so the cost is 26 brick lookups per brick rather than per-voxel neighbour lookups.
  Anchored voxels are always emitted.
- Culling applies to an object only when the splats of its surface are known to cover
  what lies behind them: the whole object is beyond the near plane, the projected
  voxel edge at the object's nearest depth is at most 24 px (the 12 px splat clamp
  covers it) and within the off-screen margin for small viewports, and a physical
  lens is not narrower than the field of view the splat size is derived from.
  Otherwise every voxel of that object is drawn as before.
- One solid layer (a 3x3x3 neighbourhood) was tried first. It left isolated 1 px
  differences at grazing silhouettes (about 0.1 px per frame over 3,000 poses), where
  a buried voxel's splat leaks past the surface splats. Two layers removed them in
  every test pose, and still drops most hidden voxels (82% of a solid 64^3 cube).
- Projection now builds a per-view `ScreenProjector` (camera basis, field-of-view
  tangent, lens shift, viewport scale) once, instead of four normalisations and a
  `tan` per projected point. `project_world_to_screen` uses the same code path.
  With culling disabled, the draw list is bit-identical to the original.
- Fewer draw items are produced, so the viewport statistics line ("Draw items: N")
  reports smaller counts. Culled voxels no longer consume `maximumDrawVoxels`, so
  scenes that previously hit the cap now draw objects that were being cut off.

## Measurements

Single-threaded, warmed medians from `dve_editor_scene_bench` (fingerprint: 400
calls; draw list: 15 rebuilds, each after a camera move). Original and revised
sources were compiled with GCC 13.3.0, C++23 and `-O2` on the same 2-core
environment. Scenes are solid 64^3-voxel cubes (512 bricks each); the draw-list case
is one cube in a 1280x720 viewport. Three runs agreed to within a few percent.

| Operation | Before | After |
|---|---:|---:|
| Scene fingerprint, 16 objects / 8,192 bricks | 368 us | 0.8 us |
| Scene fingerprint, 64 objects / 32,768 bricks | 1,503 us | 3.0 us |
| Same, three calls per frame (draw list, preview, diagnostics) | 4.5 ms | 9 us |
| Draw-list rebuild, culling off (projector only) | 37.4 ms, 120,000 items (capped) | 24.5 ms, 120,000 items (capped) |
| Draw-list rebuild, culling on | n/a | 9.2 ms, 46,144 items |

These are microbenchmark timings, not whole-editor frame rates or measured
input-to-display latency. Before this change, the fingerprint alone used more than
half of the desktop editor's 8 ms frame budget on the 64-object scene even when the
scene was idle.

An unstable `std::sort` was also tried for the depth sort (the comparator is a strict
total order, so output is identical), but it was slower than `stable_sort` on these
partly ordered lists and was not kept.

The benchmark can be run with:

```sh
cmake --build build --target dve_editor_scene_bench
./build/dve_editor_scene_bench          # 64 objects, 64^3 voxels each
./build/dve_editor_scene_bench 16 64    # objects, cube edge in voxels
```

## Validation

`dve_editor_draw_culling_tests` covers:

- revision changes for every mutator, no change for const reads and no-op edits,
  uniqueness across objects, and move construction/assignment;
- fingerprint changes on voxel edits, voxel-object replacement with identical
  content, transform and visibility changes, and stability for no-op edits;
- the culled set matching a brute-force 5x5x5 reference on 12 random objects that
  span brick boundaries and negative coordinates;
- pixel identity: culled and unculled lists painted the way `render_native_editor`
  paints voxels (filled squares and anchor crosses, far to near) over 288 views of a
  sphere, a notched cube and a porous blob, including orthographic views and a
  320x180 camera-preview-sized viewport;
- the guards: camera inside the object (perspective and orthographic), voxels larger
  than the splat clamp covers, a zoomed physical lens, and buried anchors;
- a second object that the full list cut off at the draw cap being drawn with culling.

Two deliberately broken builds were checked against these tests: removing the near
plane guard fails the pixel-identity test, and removing the `set_voxel` revision bump
fails the revision test.

Beyond the checked-in tests, during development: the bitwise exposed set matched the
brute-force reference on 560,303 voxels across 40 random objects; painting culled and
unculled lists gave 0 differing pixels across 800 views (153 million covered pixels)
of five shapes and across a further 5,254 culled frames in a 3,000-pose sweep at two
viewport sizes; and with culling disabled, draw lists and `project_world_to_screen`
results were byte-identical to the original across 460,000 items in 60 poses,
including orthographic, physical-lens, rotated-object and offset-viewport cases.

The full build (778 steps, Release, SDL3 unavailable so the desktop editor binary
was not built) completed without errors, and 248 of 249 CTest tests passed. The one
failure, `dve_cpack_test`, reports that the editor package lacks `dve_desktop_editor`,
which was not built in this environment; it is unrelated to this change. All tests
that passed on the baseline (editor, render cache, spatial, damage clip) still pass,
including the existing render-cache test that a voxel edit rebuilds the draw list.

`dve_editor_interaction_bench` on the small demo scene (824 voxels, all on the
surface, so culling removes nothing there) shows cached draw-list access at about
1.1 us median (was 2.6 us) and selection-diagnostics access at about 1.75 us (was
3.0 us), both from the cheaper fingerprint. Picking and command search, which this
change does not touch, stayed within run-to-run noise.

Desktop display latency was not measured: native SDL3 and display hardware were
unavailable here.
