# Packaging and runtime content (Phases 1–2)

This is the runtime half of shipping a game: reading content from a `.dvepak` or a loose
project folder and booting a `GameWorld` from it without the editor (Phase 1), and the
`dve_player` executable that runs it (Phase 2, [below](#the-player-dve_player)). Install/CPack
and the `dve_package_game` step are later phases.

## Content sources (`dve/content_source.hpp`, `dve_core`)

| Type | Backing | Notes |
|---|---|---|
| `LooseContentSource::open(dir)` | project folder | Every lookup stays inside the canonical root, including through symlinks. |
| `PakContentSource::open(file)` | mounted `.dvepak` | Whole-entry reads, FNV-1a checked per entry. |
| `open_content_source(path)` | either | A directory opens loose and a file opens as a pak. |

Content paths are relative, use forward slashes, and are what `dve_pack` stores (for example
`scenes/main.dvoxscene.json`). `normalize_content_path` collapses `.` and duplicate slashes.
It rejects empty or absolute paths, `..`, backslashes, drive letters, control characters and
paths over 4 KiB. Reads take a `maximumBytes` limit, which is checked before any payload is read.
Failures return a `ContentError` whose code is `InvalidPath`, `NotFound`, `LimitExceeded`, `Io`
or `IntegrityFailure`.

## Scenes (`dve/game_scene_loader.hpp`)

The shipped scene format is DVOXSCENE JSON plus `.dvox` (decision D2). Editor scenes will
need an offline export step (Phase 4), so the runtime never links `dve_editor`.

```cpp
auto content = dve::open_content_source("game.dvepak");
dve::GameWorld world(dve::create_physics3d_world(dve::Physics3DBackend::Automatic));
auto loaded = dve::load_scene_into_game_world(*content, manifest.entryScene, world);
if (!loaded) log(loaded.error.message);   // loaded.contentError.code == IntegrityFailure, ...
for (const dve::GameRenderObject& object : world.render_objects()) { /* draw */ }
```

- The manifest is validated by the same rules as `RuntimeSceneWorld`, but in memory
  (`parse_dvoxscene_manifest`). Each asset is read through the source, decoded with
  `read_dvox(span)`, and checked with `validate_dvoxscene_asset`.
- `anchored` objects get a static body; all others get a dynamic body. `structural` is passed
  through, and mass comes from the asset's per-material densities.
- An object with `generateCollision=false` becomes a *visual-only* voxel object
  (`GameWorld::spawn_visual_asset`): it renders (`GameRenderObject::collision == false`) and can
  be moved by scripts, but it has no physics body and is ignored by raycasts, overlaps, capsule
  sweeps and damage. (Before Phase 2 it was a marker, like the editor play session.)
- `parent` is hierarchy metadata and is reported in the result. Set
  `GameSceneLoadOptions::attachChildrenToParents` to turn it into GameWorld attachments.
- Loading is all-or-nothing. Every asset is validated before the first object is created, and
  if a later step fails, the objects already created are destroyed.

## Project manifest: `game.dvegame`

```
DVE_GAME 1
name=Demo
version=1.0.0
entryScene=scenes/main.dvoxscene.json
startupScript=scripts/main.lua        # optional (default scripts/main.lua if present)
settings=game_settings.txt            # optional
camera=0,1.5,6 -> 0,0.5,0             # optional fallback camera
bind.move_x=key:a/-1,key:d/+1         # optional, reserved for the player's input layer (D6)
```

The format is strict:
- The header must be exactly version 1.
- Unknown keys, duplicate keys and empty values are rejected.
- Size limits: 64 KiB per file, 4 KiB per line, 256 bindings.
- Every path must be a normalized content path.
- `entryScene` must end in `.dvoxscene.json`.

Comments are either whole lines or ` # ...` after whitespace. Use `GameManifest::parse`,
`serialize()`, `load_game_manifest(content)` and `check_game_manifest_content(manifest, content)`.

## Packing

`dve_pack <root> <out.dvepak> --all` strips the `editor/`, `docs/`, `tests/` and `artifacts/`
prefixes. It also strips any `.autosave/` directory at any depth
(`DvePakBuildOptions::editorOnlyDirectoryNames`).

## The player: `dve_player`

`dve_player` is the shipped-game executable. It is built on SDL3 and links only
`dve_player_runtime` → `dve_core`, `dve_render_bridge`, `dve_platform_sdl3` and (when present)
`dve_audio_sdl3`. It never links `dve_editor`; the `dve_player_no_editor_link` test checks the
configure-time link closure (`dve_player_link_closure.txt` in the build folder).

```
dve_player [<game.dvepak> | <project-dir>] [options]
dve_player --pak game.dvepak
dve_player --project path/to/project        # loose folder, for development
dve_player                                  # <exe>.dvepak, game.dvepak or content/game.dvepak next to the exe
```

Boot order: mount content → `game.dvegame` → `settings` (a `ui::GameSettings` file, optional) →
the `bind.*` table → a `GameWorld` with the best physics backend compiled in (Jolt, else the
reference solver) → the Lua host with the `startupScript` (Lua builds only) → the entry scene via
`load_scene_into_game_world`. The script runs before the scene loads, so its `on_scene_loaded`
handlers see the scene objects.

Exit codes: `0` ok, `1` runtime failure, `2` bad command line or no content found, `3` content
error (missing or corrupt pak, bad manifest, scene or script; the message names the failing
entry, e.g. `package entry integrity check failed`), `4` hash/reference mismatch.

### Rendering (D5)

`dve::player::IPlayerRenderer` (`dve/player/player_renderer.hpp`) is the backend seam:
`resize` → `render(PlayerRenderView)` → `present` → optional `readback`. A view is the
`GameWorld::render_objects()` snapshot, the camera and the `RenderEnvironment`.

- `make_cpu_player_renderer(IFrameBlitter*, options)` traces voxel objects with
  `ReferenceVoxelRenderer` (now multithreaded by rows, identical output for any thread count)
  and composites polygon objects with `ReferencePolygonRenderer`, then resolves to RGBA8. The
  default render size is 480×270 (× `renderScale` from the settings), upscaled with nearest
  filtering and letterboxing into the window through an SDL streaming texture
  (`SdlTextureBlitter` in `apps/dve_player.cpp`, which is the only SDL-specific part).
- The reference tracer works in voxel-index units, so the renderer uses the first voxel
  object's voxel size for the scene; objects with a different voxel size are skipped and
  counted in `PlayerRenderStats::skippedObjects` (the GPU path will handle per-instance scale).
- `--quality fast|balanced|reference` picks the lighting cost. `fast` (default) uses hard sun
  shadows and hemisphere ambient; `balanced` adds the environment's GI mode capped at 2 samples;
  `reference` uses the environment unchanged (soft shadows, 4-sample one-bounce GI).
- A future Vulkan/D3D12 presenter implements `IPlayerRenderer` directly (render from
  `render_objects()` and present to its own swapchain), or implements only `IFrameBlitter` to
  keep CPU tracing but present through the GPU.

On an 8-core box without a GPU, the sample scene at 480×270 takes about 32 ms per frame with all
cores (`fast`), about 63 ms with `--threads 4`, about 56 ms with `balanced` and about 100 ms with
`reference`.

### Game loop, camera and input (D6)

- Fixed timestep: `FixedStepClock` (1/60 s, 0.25 s clamp, at most 8 steps per frame). Each step
  pushes input into the world and calls `GameWorld::tick` (which runs Lua `on_tick`). One render
  per frame.
- Camera: a script can set the `player_camera_eye` / `player_camera_target` global vectors.
  Otherwise the camera is the manifest `camera=`, and otherwise a default. The settings'
  `fieldOfViewDegrees` is the horizontal FOV.
- Bindings: `bind.<name>=<source>[,<source>...]` with `key:<sdl key name>[/value]`,
  `gamepad:<button>[/value]`, `gamepad:<axis>[/scale]` (leftx, lefty, rightx, righty,
  lefttrigger, righttrigger; 0.15 dead zone) and `mouse:<button>[/value]`. A binding with any
  explicit value or a gamepad axis is an axis (a clamped sum in [-1, 1]). Otherwise it is an
  action. Scripts read them with `world.get_axis(name)` and `world.is_action_pressed(name)`.
  Escape or closing the window quits.

### Lua (D4)

In Lua builds, the startup script and every `require` load through the `ContentSource`, so the
same code runs from a loose folder or a pak. `require("a.b")` tries `scripts/a/b.lua`,
`scripts/a/b/init.lua`, `a/b.lua` and `a/b/init.lua`. `world.spawn_asset` and
`camera_load_sequence` also read content paths through the source. Builds without Lua skip the
script with a log line (`--no-script` skips it on purpose).

### Audio

Video and audio are initialised separately (`SDL_InitSubSystem(SDL_INIT_AUDIO)` after video).
If no audio device can be opened, the player logs
`audio unavailable, continuing without sound` and runs silently (`--no-audio` skips audio).
The desktop editor's `--smoke` mode follows the same rule and prints `audio=unavailable (...)`
instead of failing.

### Deterministic runs and screenshots

`--frames N --fixed-dt s` runs exactly N fixed steps with one render each, ignoring wall-clock
time. `--hold key@from-to` holds a key over a frame range, and `--headless` skips the window.
`--hash` prints `framebuffer_fnv` (FNV-1a of the last RGBA8 frame). `--expect-hash h[,h]` exits
4 unless the hash matches, or unless the frame is within tolerance of `--reference-image`
(box-downsampled PPM; mean |Δ| ≤ 1.5 and ≤ 1 % of pixels off by more than 24). `--screenshot`
writes the frame as PNG or PPM, and `--window-screenshot` writes the presented window as BMP.
The output also has `key=value` lines (renderer, video/audio driver, physics, script,
`render_ms_*`, `frame_ms_avg`, `object.<name>=x,y,z`) for tests and profiling.

Golden hashes live in `tests/data/player_golden/expected_frame_hashes.txt`, keyed by
`<compiler>-<major>-<lua|nolua>`. An unknown compiler falls back to the reference image and
prints the hash to add.

### Presets and the sample game

- `linux-gcc-player-release`: static SDL 3.4.12 (`DVE_FETCH_SDL3` + `DVE_SDL3_PREFER_FETCH`,
  and no system SDL3_ttf), Lua, Jolt 5.6.0 and `BUILD_SHARED_LIBS=OFF`. `dve_player` has no
  `libSDL3.so` dependency. Other presets build `dve_player` against whatever SDL3 they find
  (`DVE_BUILD_PLAYER`, default ON), and it falls back to the reference physics and no scripts
  when Jolt or Lua is absent.
- `tests/data/player_sample` is a small game (ground, tower, crate and a visual-only spinner
  driven by `scripts/main.lua` + `require("spinner")`). The `.dvox` files are generated by
  `dve_player_sample_generator` (`dve_player_sample_assets_up_to_date` checks them), and the
  `dve_player_sample_pack` test fixture packs it with `dve_pack --all`. Tests:
  `dve_player_runtime_tests` (in-process: bindings, clock, boot, loose == pak, bad content) and
  `dve_player_{smoke, loose_matches_pak, missing_pak, corrupt_pak, no_content, default_pak,
  no_audio, input}` (CLI).
