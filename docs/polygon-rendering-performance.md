# Polygon rendering performance and settings

This batch improves the active CPU player/reference polygon renderer. The native
editor viewport remains a voxel preview; this does not add a native polygon/GPU
viewport or connect the separate `polygon.instancing` control.

## Rendering changes

- Validate each distinct cooked asset once per render call, including its content
  hash. Validation is not skipped across frames, so an in-place edit without a new
  hash is still rejected by the reference renderer.
- Derive bounds from current vertices, transform them with the current rigid pose,
  and conservatively test all six planes of the renderer's perspective frustum.
  Touching/intersecting and uncertain bounds remain visible. Rejection happens
  before mip generation and triangle submission. Turning culling off retains the
  existing visibility, minimum-size, projection, backface and depth rules.
- Share immutable mip chains across instances and, with a persistent cache, across
  frames. Keys use validated asset content hashes, rather than borrowed asset
  pointers. A texture/content change publishes a new key. Textures disabled or
  absent generate no mipmaps.
- Retain at most 64 MiB of mip RGBA payload and 128 asset entries by default. Least
  recently used entries are evicted; oversized chains remain frame-local and are
  still built only once per asset per call. Transparent triangles keep shared
  ownership until rasterization finishes, even after eviction. These limits cover
  retained mip payload, not total scene textures or all transient render memory.
- The CPU player shares one scaled cooked copy per content hash and voxel-unit
  scale, retaining independent transforms, tints and object IDs in draw instances.
  A scale change rebuilds the copies; absent assets are pruned every frame. This
  replaces one complete mesh/image copy per object and the previous quadratic
  live-object lookup during pruning. Unhashed caller assets are fingerprinted.

`PolygonRenderCache` belongs to one viewport/render thread. `clear()` releases
retained state; the CPU player clears it when no polygon objects remain. Cache
entries own prepared texture data, so they do not dereference assets after their
owners unload them. Required frame-local data and pinned transparent data may
temporarily exceed the retention budget. The separate `render.texture_budget_mb`
setting still requires a scene residency manager and remains unapplied.

## Settings and application boundary

| Setting | Behavior |
| --- | --- |
| `polygon.frustum_culling` | Enable/disable conservative object rejection in CPU player/reference rendering. Off does not disable ordinary raster clipping or depth testing. |
| `polygon.lod_bias` | Effective projected diameter is `diameter * 2^(-bias)`. Positive chooses coarser supplied levels; negative chooses finer levels. The valid range is -4 to 4. No supplied levels means the base mesh remains unchanged. |

LOD means **level of detail**: separately authored versions of a mesh. Each level
declares a minimum projected diameter in pixels; higher thresholds indicate finer
geometry. Below every threshold, use the coarsest available level instead of
returning to the full-detail base mesh. A persistent cache supplies a 10% hysteresis
band to prevent oscillation around thresholds. Changing bias, base content or the
available level/threshold resets the relevant history. History is bounded to 4096
objects and pruned when objects stop using it. The selected mesh's actual vertex
bounds are used for culling. Selection IDs and source/collision geometry do not
change when rendering a different level.

Project preferences are exported to the game's existing defaults file:

```sh
dve_export_scene scenes/main.dvescene game/scenes/main.dvoxscene.json \
  --project-root . --game-settings-output game/game_settings.txt
```

Use `settings=game_settings.txt` in `game/game.dvegame`, as in the existing sample.
The exporter reads `<project-root>/.dve/project/editor_settings.txt`, resolves the
two polygon settings, and preserves the other recognized game defaults in an
existing output file. Invalid preferences/defaults fail explicitly. Writes use a
temporary file and platform replacement, without deleting the previous file
first. Omitting `--game-settings-output` leaves existing scene-export behavior
unchanged. An explicit `--project-root` is required for this option.

Game settings use optional `polygon.frustum_culling` and `polygon.lod_bias` keys;
old settings files keep defaults of true and zero. The player reads the file at
boot, publishes those values in `PlayerRenderView`, and consumes them every render.
Project Apply therefore takes effect after export and the next game boot, not in
the native voxel editor viewport. User/Session preferences are not baked by the
CLI; SDK callers can resolve those layers with `polygon_game_settings()`.

C++ hosts can supply authored `GameRenderObject::polygonLods` spans and must keep
the levels/assets alive through `render()`. Their scaled copies are shared and
their transformed bounds stay current. This batch does not generate simplified
meshes, add LOD references to `.dmesh`/scene packaging, or expose LOD assignment to
Lua. Single-mesh scenes consequently see no visual effect from changing LOD bias.
The controls are marked connected for these supported consumers; that is not
full acceptance certification for every editor/host path.

## Verification and measured effect

All eight targeted local CTest checks passed: polygon performance, Project export
to loose/packed player settings, existing polygon rendering, hybrid reference
rendering, player runtime, game UI, editor scene export and the settings reference
audit. Platform-boundary, whitespace and source-manifest checks also pass. The
full engine suite and Windows execution are left to CI.

Behavior checks compare exact color, depth, material and object-ID buffers with
culling on/off and cache reuse; cover rotated/intersecting geometry, stale metadata
bounds, all six planes, texture edits/corrupt hashes, bounded eviction with pinned
transparent data, disabled textures, LOD sign/fallback/hysteresis/live changes,
player mesh sharing and unload, settings validation and atomic replacement.
The export check updates an existing defaults file twice and boots both loose and
packaged content, verifying that unrelated recognized defaults survive.

The bounded benchmark uses 64 instances of one quad, one 512×512 RGBA texture, a
32×32 target, three warm-up frames and the median of ten measured frames. The old
renderer was compiled from main `5569fef` with the same benchmark object and
Release flags, replacing only the polygon renderer object in the link.

| Measurement | Main `5569fef` | This batch |
| --- | ---: | ---: |
| Median warmed render | 154.00 ms | 1.48 ms |
| Exact framebuffer hash | 4258369384175444764 | 4258369384175444764 |
| Validations per measured frame | 64 (source loop) | 1 (counter) |
| Mip chains built per measured frame | 64 (source loop) | 0 (counter) |
| Warm mip hits | Not instrumented | 64 |
| Retained mip payload | No persistent mip cache | 1,398,100 bytes |

This is approximately 104× faster for repeated preparation in this synthetic
fixture. It is not an overall game frame-rate estimate. Heavy shading, unique
assets, cold-cache frames, voxel tracing and other hosts can dominate real scenes.
Some per-frame bookkeeping still allocates; this is not a zero-allocation claim.

Reproduce on a Linux Ninja Release build with exported compile commands:

```sh
cmake --build build --target dve_polygon_render_bench
python scripts/benchmark_polygon_before_after.py --build build --baseline 5569fef
```

The script uses a temporary directory, compiles the historical renderer with the
current compile/link commands, verifies equal framebuffer hashes and reports
timings. Older source has no performance counters; its zero-initialized new fields
are deliberately omitted from the comparison output.
