# Packaging and runtime content (Phases 1–4)

This is the runtime half of shipping a game: reading content from a `.dvepak` or a loose
project folder and booting a `GameWorld` from it without the editor (Phase 1), and the
`dve_player` executable that runs it (Phase 2, [below](#the-player-dve_player)), and installing
and packaging the engine (Phase 3, [below](#installing-and-packaging-phase-3)), and turning a
game project into a shippable folder with its third-party notices (Phase 4,
[below](#shipping-a-game-phase-4)).

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

The shipped scene format is DVOXSCENE JSON plus `.dvox` (decision D2). Editor scenes are
converted offline by `dve_export_scene` ([Phase 4](#editor-scenes-dve_export_scene)), so the
runtime never links `dve_editor`.

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
- Optional per-object `extensions` block (versioned, `"version": 1`; a newer or missing version,
  or an unknown field, is rejected rather than half-understood):
  - `"geometry": "polygon"`: `file` is a `.dmesh`. It spawns through
    `GameWorld::spawn_cooked_polygon_asset` (static when anchored, dynamic otherwise), or
    `spawn_visual_polygon_asset` when `generateCollision=false`. The CPU player renderer draws
    polygons with `ReferencePolygonRenderer` after the voxels. Only `load_scene_into_game_world`
    loads polygon objects; the path-based `RuntimeSceneWorld` rejects them.
  - `"components"`: serialized `dve::Component`s (`id`, `type`, `enabled`, typed `properties`:
    `{"bool":b}`, `{"int":"<decimal string>"}` so 64-bit values survive JSON, `{"float":d}`,
    `{"string":s}`, `{"float3":[x,y,z]}`, `{"quat":[x,y,z,w]}`), added with
    `GameWorld::add_component`. Tags, groups and the layer travel as a `dve.membership` component.
  - `"attachment"`: `socket`, `inheritPosition`, `inheritRotation`. It needs a `parent`.
- `parent` is hierarchy metadata unless the object has an `attachment` extension (always
  attached) or `GameSceneLoadOptions::attachChildrenToParents` is set (every parented object is
  attached, except an anchored child of a static parent). `dve_player` sets
  `attachChildrenToParents`, so children move with their parents.
- Physics interplay of attachments: a child with a body is snapped to its parent every tick and
  gets the parent's rigid-motion velocity (it used to keep its own velocity and accumulate
  gravity between snaps). Parent and child bodies never collide with each other: GameWorld
  disables the pair through `IRigidBodyWorld::set_pair_collision_enabled` (Jolt uses a contact
  validation filter; the reference solver has no body-body contacts). The child still collides
  with everything else, but it cannot push its parent: contacts on the child do not propagate
  to the parent's body.
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

### Save games

Details are in [SAVE_GAMES.md](SAVE_GAMES.md).

- The `quicksave` / `quickload` actions default to F5 / F9. Rebind them with
  `bind.quicksave=...` / `bind.quickload=...` in `game.dvegame`.
- Slots are `<slot>.dvesave` in `$XDG_DATA_HOME/dve/<game>/saves` (or
  `~/.local/share/dve/<game>/saves`; `%APPDATA%` on Windows, `~/Library/Application Support`
  on macOS). `--save-dir <dir>` overrides the folder.
- `--load <slot|file.dvesave>` resumes a save at start-up. A corrupt, truncated or foreign save
  exits `3`, naming the problem.
- Scripts can save and load with `world.save_game(slot)` / `world.load_game(slot)` and keep
  their own state with `world.on_save` / `world.on_load`.
- A save holds the whole GameWorld: objects, the voxels each one has left after destruction
  (a delta against the source asset in the pak where possible), the physics bodies, timers,
  pools and the script state. It is a hashed, versioned `DVESAVE1` document with migrations.

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
`render_ms_*`, `frame_ms_avg`, `object.<name>=x,y,z`, and for save games `save_dir=`,
`saved=` / `saved_bytes=` / `saved_tick=` / `saved_framebuffer_fnv=`, the same for `loaded`,
and `resumed_*` after `--load`) for tests and profiling.

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
  driven by `scripts/main.lua` + `require("spinner")`). B blasts the tower, F5 / F9
  quicksave / quickload, and 1 / 2 save / load `slot1` from Lua. The `.dvox` files are generated by
  `dve_player_sample_generator` (`dve_player_sample_assets_up_to_date` checks them), and the
  `dve_player_sample_pack` test fixture packs it with `dve_pack --all`. Tests:
  `dve_player_runtime_tests` (in-process: bindings, clock, boot, loose == pak, bad content) and
  `dve_player_{smoke, loose_matches_pak, missing_pak, corrupt_pak, no_content, default_pak,
  no_audio, input, save_load}` (CLI). `dve_player_runtime_tests` also covers save / load /
  quickload in process. `dve_player_save_load` saves in one process and `--load`s in another,
  and checks that the loaded frame and the continued frame hashes match.

## Installing and packaging (Phase 3)

`cmake/DveInstall.cmake` (install rules, exported package, bundling), `cmake/DveCPack.cmake`
(CPack) and `cmake/DveVersion.cmake` (the version) are included from the top-level
`CMakeLists.txt`. Turn it all off with `-DDVE_INSTALL=OFF`.

```sh
cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release
cmake --install out/build/linux-gcc-release --prefix /opt/dve               # everything
cmake --install out/build/linux-gcc-release --prefix /opt/dve --component Runtime
cd out/build/linux-gcc-release && cpack -G "TGZ;DEB"                         # packages
```

### Components and packages (D7)

| Component | Contents | Package |
|---|---|---|
| `Runtime` | `bin/dve_player` | `dve-runtime` |
| `RuntimeDeps` | `lib/dve/*.so*`: the player's non-system shared libraries | `dve-runtime` (archives only) |
| `Editor` | `bin/dve_desktop_editor`, `bin/dve_native_editor_x11`, `share/dve/assets/` | `dve-editor` |
| `EditorDeps` | `lib/dve/*.so*` for the editor | `dve-editor` (archives only) |
| `Tools` | `bin/dve_pack`, `bin/dve_export_scene`, `bin/dve_package_game`, `bin/dve_cook_*`, `bin/dve_asset_index`, `bin/dve_prefab_tool` | `dve-tools` |
| `ToolsDeps` | `lib/dve/*.so*` for the tools | `dve-tools` (archives only) |
| `Development` | `include/dve/**` (with the generated `version.hpp`, `build_config.hpp`), `lib/libdve_*.a`, `lib/dve/third_party/*.a`, `lib/cmake/dve/` | `dve-dev` |

Only executables that exist in the build are installed; for example, a build without SDL3 has no
`Runtime` or `Editor` component. Every component also installs its
`share/doc/dve/THIRD_PARTY_NOTICES-<component>.txt` (Phase 4).

- **Sample maps (D8).** `assets/audio/sample_maps` is never installed, because its provenance is
  undocumented. `-DDVE_INSTALL_SAMPLE_MAPS=ON` adds it to the `Editor` component only; the
  `Runtime` component never contains assets. The install-tree and cpack tests fail if a sample
  map ends up in any package.
- **Versioning.** `project(... VERSION x.y.z)` is the only place the version is written.
  `cmake/DveVersion.cmake` generates `dve/version.hpp` (`DVE_VERSION_MAJOR/MINOR/PATCH/STRING`,
  `DVE_VERSION_NUMBER`, `DVE_GIT_DESCRIBE` from `git describe --always --dirty` at configure
  time, and `dve::kVersionString`). `dve_player --version`, `dveConfigVersion.cmake` and the
  CPack metadata all use it. `dve_version_consistency` fails if `release-manifest.json` or the
  README title drift from it.

### RPATH and bundled libraries (Linux)

Installed executables get `INSTALL_RPATH $ORIGIN/../lib/dve`; `dve_player` also gets
`$ORIGIN/lib/dve`, the layout of a shipped game folder (Phase 4). They are linked with
`--disable-new-dtags`, so this is a `DT_RPATH` rather than a `DT_RUNPATH`: the loader applies it to
every library in the process, so the dependencies of a bundled library (for example
`libsndfile` → `libFLAC`) also resolve from `lib/dve`, without `patchelf`. (A `DT_RUNPATH` only
covers the executable's own direct dependencies.) The trade-off is that `LD_LIBRARY_PATH`
cannot override the bundled copies.

`install(TARGETS ... RUNTIME_DEPENDENCY_SET)` + `install(RUNTIME_DEPENDENCY_SET)` copy every
shared library an installed executable needs into `lib/dve`, except the libraries every
desktop provides and that must match the running system. Those are the
`DVE_RUNTIME_DEPENDENCY_SYSTEM_EXCLUDES` name patterns (matched before resolution, so their own
dependencies are not walked):
- glibc and the C++ runtime (`libc`, `libm`, `libstdc++`, `libgcc_s`, `ld-linux`, …);
- the X11/XCB/Wayland/GL/EGL/Vulkan/DRM stacks;
- the audio servers (ALSA, PulseAudio, PipeWire, sndio);
- dbus/udev/systemd, zlib, glib, libffi/expat/pcre2 and similar;
- the font stack (freetype, harfbuzz, fontconfig, libpng16, brotli, bz2);
- `libjack` and Berkeley DB `libdb` (Phase 4; `DVE_INSTALL_BUNDLE_JACK=OFF` by default). libjack
  must match the JACK server on the machine, and libdb's Sleepycat license reaches the software
  that uses it (see [licenses](#third-party-licenses-and-what-to-review)). They are only reachable
  through RtMidi, so this affects the editor.

What gets bundled depends on the preset. With `linux-gcc-release`, the Debian `libSDL3.so.0`
itself links X11, Wayland, PulseAudio, PipeWire and sndio directly, so a TGZ built from it needs
those on the target machine. The portable choice for games is `linux-gcc-player-release`
(static SDL 3.4.12, decision D3), which only bundles Lua, RtMidi and libsndfile with its codecs.
libsndfile is DVE's own build without MP3 support in every preset (see
[libsndfile without MP3](#libsndfile-without-mp3)), so `libmpg123` and `libmp3lame` are never bundled.

### DEB packages

The `.deb` files install into `/usr` and do **not** contain `lib/dve`: the `*Deps` components
are dropped for the DEB generator (`DveCPackProjectConfig.cmake`), and the packages depend on the
distribution's libraries instead. `Depends` is computed by `dpkg-shlibdeps`
(`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`) when `dpkg-dev` is installed. Without it, a hand-written list
is used (`libc6, libstdc++6, libgcc-s1`, plus `libsdl3-0` / `liblua5.4-0` when they are
linked dynamically), and configure says so. `dve-dev` holds static archives, which shlibdeps
cannot scan, so its `Depends` lists the `-dev` packages that own the libraries and CMake
packages the exported targets reference (found with `dpkg -S` at configure time, e.g.
`libsdl3-dev, librtmidi-dev, libsndfile1-dev`).

On Windows, `CPACK_GENERATOR` is `ZIP;NSIS`, and the DLLs the executables need are installed next
to them in `bin/` (see [Windows](#windows)). CI builds the ZIP (`cpack -G ZIP`); NSIS is configured
but not tested.

**License (D1): MIT.** The engine is licensed under the MIT License (root `LICENSE`,
`Copyright (c) 2026 Benjamin Schulz`). `cmake/DveLicense.cmake` installs it into every package
group from its main component (`Runtime`, `Editor`, `Tools`, `Development`) as
`share/doc/dve-<group>/LICENSE`, together with a Debian machine-readable copyright file
`share/doc/dve-<group>/copyright` (`/usr/share/doc/dve-<group>/copyright` in the `.deb`, as Debian
policy expects; `License: Expat` is Debian's name for MIT). One folder per package keeps the
`.deb` files from owning the same path; the hidden `*Deps` components always ship in the archive of
their main component. `CPACK_RESOURCE_FILE_LICENSE` is the root `LICENSE` (shown by the NSIS
installer). Each package also carries its component's `THIRD_PARTY_NOTICES-<component>.txt`
(Phase 4), which starts with the engine's license.

### The `dve` CMake package

```cmake
find_package(dve 2.35 REQUIRED COMPONENTS player_runtime)   # SameMinorVersion: 2.35.x only
add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE dve::player_runtime)
```

- **Targets:** `dve::core`, `dve::platform`, `dve::rhi`, `dve::render_bridge`,
  `dve::audio_synth`, `dve::player_runtime`, and, in SDL3 builds, `dve::platform_sdl3` and
  `dve::audio_sdl3`. `COMPONENTS` are these names without the namespace. The editor and asset
  pipeline libraries are not exported.
- **Variables:**
  - `dve_VERSION`, `dve_VERSION_GIT_DESCRIBE`;
  - `dve_HAVE_LUA`, `dve_HAVE_JOLT`, `dve_GEOMETRY_MODE`;
  - `dve_SDL3_BUNDLED` (SDL was fetched and is exported with the package);
  - `dve_BUILD_CONFIG_DEFINES`;
  - `dve_CXX_COMPILER_ID` and `dve_CXX_COMPILER_VERSION`. Use the same compiler, because the
    static libraries use C++23.
- `tests/package_consumer/` is a 60-line example game, and `dve_package_consumer_test` builds it
  against an install prefix.

#### Option-dependent defines and dependencies (the fragile part)

**Public compile definitions.** `dve_core` has PUBLIC compile definitions that change public
headers and class layouts:
- `DVE_ENABLE_DEFORMABLE_RUNTIME` and `DVE_ENABLE_CPU_HAIR` add `GameWorld` members;
- `DVE_ENABLE_FLIP_LIQUIDS`;
- `DVE_GEOMETRY_MODE_*`;
- `DVE_HAVE_LUA`, `DVE_HAVE_JOLT`, `DVE_HAVE_MANIFOLD`, …

A consumer compiled with a different set would violate the ODR, and it would only show up at run
time. How this is handled:
- The definitions are part of `dve::core`'s exported `INTERFACE_COMPILE_DEFINITIONS`, so anything
  that links the imported targets gets exactly the set the libraries were built with.
- `dve/build_config.hpp` (generated) records the set and `#error`s if the including translation
  unit disagrees in either direction. The consumer test includes it and also checks
  `PlayerApp::scripting_compiled_in()` against it. Code that bypasses the imported targets (for
  example, hand-written `-I`/`-l` flags) should include it first.
- Jolt's own PUBLIC compile options (`-mavx2`, `-mfma`, … and its `JPH_*` definitions) travel with
  `dve::third_party_Jolt` for the same reason.

**Dependencies.** `DveInstall.cmake` walks the link interface of the exported targets. For
static libraries this includes their PRIVATE dependencies as `$<LINK_ONLY:...>`. Each dependency
is handled as follows:

| Dependency kind | Examples | Handling |
|---|---|---|
| Built in this tree | vendored `manifold`; fetched Jolt (`DVE_FETCH_JOLT`), SDL3-static + `SDL3_Headers` (`DVE_FETCH_SDL3`), RtMidi, Box2D/Box3D | Exported with our targets as `dve::third_party_<name>`, archives in `lib/dve/third_party`. So a fetch build yields a self-contained package. |
| Installed package (namespaced import) | `SDL3::SDL3`, `Jolt::Jolt`, `RtMidi::rtmidi`, `Threads::Threads` | `find_dependency(<Pkg>)` in `dveConfig.cmake`, with the build-time `<Pkg>_DIR` as a hint. |
| Local import helper | `PkgConfig::DVE_LUA`, the Lua ABI fallback `dve_lua54_runtime`, `dve_rtmidi_imported`, legacy Jolt/Box pairs | Recreated under the same name from the recorded library files. A file missing on the consumer machine is looked up with `find_library()`, else `find_package(dve)` fails with a message naming it. No include directories are recreated, because no public DVE header includes a third-party header (the install-tree test enforces this). |
| Plain library file | the system `libsndfile.so` (`DVE_FETCH_SNDFILE=OFF`) | Kept as the absolute path. `dve-dev` depends on the owning `-dev` package. |
| Library built here and bundled | `dve_sndfile_imported` (DVE's libsndfile, `DVE_FETCH_SNDFILE`) | Recreated with the location `${PACKAGE_PREFIX_DIR}/lib/dve/libsndfile.so.1` (the bundled copy from the `*Deps` components). If that file is missing, `find_library(libsndfile.so.1)` uses the system one. `dve-dev` depends on `libsndfile1`. |

The package is relocatable: the install-tree test fails if any `lib/cmake/dve` file mentions the
source or build tree. If a dependency cannot be exported (for example, a third-party target built
here as a shared library), configure prints a warning and skips the `Development` component
instead of failing the build.

Paths exercised by the tests:
- system SDL3 + manifold (`linux-gcc-release`);
- `PkgConfig::DVE_LUA` + system RtMidi (`linux-gcc-lua-release`);
- fetched static SDL3 + fetched Jolt (`linux-gcc-player-release`).

The fetched-RtMidi, Box2D/Box3D and legacy include/library paths follow the same rules but are
not covered by a preset.

### Tests

| Test | What it checks |
|---|---|
| `dve_version_consistency` | `release-manifest.json`, the README title and the generated `version.hpp` match `project(VERSION)`. |
| `dve_install_tree_test` | Installs every component into `install_tests/prefix` in the build folder and checks the expected files and that no sample maps were installed. It also checks the executables: `readelf -d` shows the `$ORIGIN` RPATH, and `ldd` finds every non-system library inside the prefix (none are "not found"). It then runs the installed `dve_player --pak <sample> --frames 30 --hash` from another directory with `LD_LIBRARY_PATH` unset: same hash as the build-tree player, and a golden hash when one is known. Finally it repacks the sample with the installed `dve_pack` and runs the installed editor's `--smoke`. |
| `dve_package_consumer_test` | Configures, builds and runs `tests/package_consumer` against that prefix with `find_package(dve X.Y)` (10 headless frames of the sample pak). It checks that `X.Y.Z` is accepted and `X.(Y±1)` is rejected (SameMinorVersion). |
| `dve_cpack_test` (label `slow`) | `cpack -G "TGZ;DEB"`, then checks each archive's listing and each `.deb`'s `dpkg-deb -c` / `-f` (package name, version, `Depends`, no `lib/dve` in the DEBs, no sample maps). It also requires `share/doc/dve-<group>/LICENSE` (identical to the root `LICENSE`) and `copyright` in every archive, and a DEP-5 `/usr/share/doc/dve-<group>/copyright` with `Copyright: 2026 Benjamin Schulz` and the MIT (Expat) text in every `.deb`. It writes a size summary to `install_tests/cpack/summary.txt`. |

## Shipping a game (Phase 4)

Phase 4 turns a game project (a folder with `game.dvegame`) into a folder or `.tar.gz` that a
player can unpack and run: no editor, no engine install, nothing on `LD_LIBRARY_PATH`.

```
<Game>/
  <Game>                     dve_player, renamed and stripped (RPATH $ORIGIN/lib/dve)
  game.dvepak                cooked content: game.dvegame, scenes, .dvox, scripts, audio
  lib/dve/*.so*              only the bundled libraries this executable needs
  THIRD_PARTY_NOTICES.txt    the Runtime notices (checked to cover every lib/dve file)
  DVE-LICENSE.txt            the engine's MIT license (share/doc/dve-runtime/LICENSE, or
                             --engine-license)
  LICENSE*/COPYING*          the game's own license, copied from the project folder if it has any
  build-info.json            engine version/git describe, game name/version, file list
```

`dve_player` with no `--pak`/`--project` looks for `game.dvepak` next to its executable, so
running `./<Game>` from any directory starts the game.

### Editor scenes: `dve_export_scene`

```
dve_export_scene <scene.dvescene> <out.dvoxscene.json> [--materials f.dvematerials]
                 [--objects-dir d] [--name n] [--project-root d] [--gabor-opacity t]
                 [--strict] [--quiet]
```

A Tools executable (it links `dve_editor`; the player does not). It writes a DVOXSCENE v1
manifest and one `.dvox` or `.dmesh` per object in `<stem>.objects/`.

| Editor | DVOXSCENE |
|---|---|
| object id, name | `id`, `name` (objects in id order) |
| world transform | rigid column-major `transform` (the editor's world matrix) |
| voxels | `<stem>.objects/<id>.dvox` |
| `.dmesh` object (`sourceAsset`) | validated and copied to `<stem>.objects/<id>.dmesh`, `extensions.geometry = "polygon"`. The source path is resolved against `--project-root` (default: the nearest folder above the scene that holds `project.dveproject`). |
| 3D text (`text3d`) | baked to voxels at the object's voxel size (see below) |
| Gabor volume | baked to visual-only voxels (see below) |
| `anchored` / `structural` / `collisionEnabled` | `anchored` / `structural` / `generateCollision` |
| parent | parent index |
| attachment (socket, inherit flags) | `extensions.attachment`: the player attaches the child, so it follows its parent |
| components | `extensions.components`: every `dve.*` type the default component registry validates, plus custom (non-`dve.`) types as-is. Unknown `dve.*` types are dropped with a warning; `dve.prefab_instance` is dropped silently (prefabs are flattened). |
| tags, groups, layer | a `dve.membership` component (`GameWorld::find_by_tag/group/layer` work in the player) |
| material ids | material table from the `EditorMaterialLibrary` (default or `--materials`), so colours and densities match the editor; unknown ids get a neutral material and a warning |

Bakes (reported as notes, which `--strict` accepts):

- **3D text.** The runtime has no Slug text renderer, so the glyphs are voxelized: a voxel
  column is inside when its centre is inside the glyph outline (the style's non-zero or even-odd
  fill rule over the outline edges of the cooked side mesh; the `.dtext` glyph curves are not kept after a reload).
  The extrusion depth is split into layers centred on the text plane. The front and back layers
  use the face colour, the inner layers the side colour, and the physical properties come from
  the style's material ids in the library. Thin strokes narrower than a voxel disappear, so use
  a voxel size well below the stroke width. Collision and anchoring follow the object's flags.
- **Gabor volumes.** The runtime has no volumetric renderer and a Gabor field has no rigid
  body, so it is voxelized: a voxel is filled when the field's density at its centre makes a
  voxel-thick slab at least `--gabor-opacity` opaque (default 0.5, i.e. `density >= -ln(1-t) /
  voxelSize`). The result is one visual-only, non-structural material (tint times the mean
  albedo, plus emission). It never has collision. A field that never reaches the threshold is
  skipped with a warning.

Not exported, each with a warning: hidden and empty objects, `.dmesh` objects whose file is
missing or invalid, and unknown engine component types. Prefab links are flattened to their
current voxels. `--strict` makes any warning an error, and then no manifest is written.
Re-exporting is byte-identical and removes stale `.dvox`/`.dmesh` files.

### `dve_package_game` / `dve_add_game_package()`

```
dve_package_game --project <dir> --output <dir> [--name N] [--tgz] [--zip] [--verify]
                 [--runtime-prefix <install prefix> | --build-dir <build dir> [--config <cfg>]]
                 [--materials f] [--strict-export] [--include-sample-maps] [--no-strip] [--keep-work]
```

Installed as `bin/dve_package_game` (Tools component; the source is `scripts/dve_package_game.py`,
Python 3). With `--runtime-prefix` (the default is the prefix it is installed in) it uses that
install's `dve_player`, `lib/dve`, `dve_pack`, `dve_export_scene` and notices; with `--build-dir`
it installs the Runtime component of a build tree into a temporary prefix first. Steps:

1. Validate `game.dvegame` (name, version, entry scene).
2. Stage the project, excluding `*.autosave`, `.git`, the top-level `editor/`, `docs/`, `tests/`
   and `artifacts/` folders, `project.dveproject`, backup `*.objects.rN/` folders, and
   `audio/sample_maps` (unless `--include-sample-maps`, decision D8).
3. Export every `.dvescene` that has no cooked `<stem>.dvoxscene.json` with `dve_export_scene`.
4. `dve_pack --all` the staged folder into `game.dvepak`.
5. Copy the player as `<Game>` (stripped unless `--no-strip`) and the part of `lib/dve` that
   its `DT_NEEDED` closure uses.
6. Copy the Runtime notices as `THIRD_PARTY_NOTICES.txt` and fail if a shipped `.so` is not
   covered by it.
7. Write `build-info.json`; with `--tgz`, write a reproducible
   `<Game>-<version>-linux-x86_64.tar.gz` next to the folder (sorted entries, `SOURCE_DATE_EPOCH`
   or 0 as mtime, root owner).
8. With `--verify`, run the packaged executable headless for 2 frames with `LD_*` removed from
   the environment.

It refuses to replace a non-empty output folder that it did not create.

From CMake, both in the engine tree and through the installed package:

```cmake
find_package(dve 2.35 REQUIRED)
dve_add_game_package(my_game_package
    PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/game
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/ship/MyGame
    [NAME MyGame] [TGZ] [ZIP] [VERIFY] [ALL] [RUNTIME_PREFIX <prefix>] [MATERIALS <file>]
    [EXTRA_ARGS ...])
```

In the engine tree, `cmake --build <build> --target dve_sample_game_package` packages
`tests/data/player_sample` into `<build>/game_packages/Player_Sample` (with a `.tar.gz` on Linux and
a `.zip` on Windows). The Windows differences are described in [Windows](#windows).

Which libraries end up in `lib/dve` depends on the preset (see
[RPATH and bundled libraries](#rpath-and-bundled-libraries-linux)). With `linux-gcc-release` the
sample game folder is about 12.1 MiB (6.4 MiB as `.tar.gz`): a 3.9 MB executable, a 27 KB pak,
~310 KB of notices, and libSDL3, DVE's libsndfile and its codecs (FLAC, vorbis, vorbisenc, ogg,
opus). Debian's libopus alone is 3.5 MB. With `linux-gcc-lua-release` it is 12.5 MiB
(adds liblua5.4). With `linux-gcc-player-release` (static SDL 3.4.12 and Lua 5.4 bundled) it is
15.5 MiB (7.9 MiB as `.tar.gz`): the executable grows to 10 MB, but libSDL3 and its X11/Wayland/
PulseAudio dependencies are no longer needed from the system.

### Third-party notices

- `third_party/notices/manifest.json` lists every third-party component the engine can link or
  bundle, how to recognise it (sonames, CMake targets, static archives, source paths) and where
  its license text comes from (the Debian `/usr/share/doc/<pkg>/copyright` of the installed
  package, a file in the repository, the fetched source's `LICENSE` next to the target's
  `SOURCE_DIR`, or `DVE_STEAM_AUDIO_ROOT`). Each entry has `copyleft` and `review` notes.
- `cmake/DveNotices.cmake` writes, per component, the executables, their link closure (targets
  and library files) and the system-exclude patterns to `notices/inputs/<Component>.json`. The
  `dve_third_party_notices` target (in `ALL`) runs `tools/generate_third_party_notices.py` to write
  `notices/THIRD_PARTY_NOTICES-<component>.txt`, which is installed to `share/doc/dve`.
- The generator follows the same rules as `install(RUNTIME_DEPENDENCY_SET)`: it walks `readelf`
  `DT_NEEDED` recursively, prunes the system-exclude patterns, and resolves the rest with `ldd`.
  Imported targets count through their files; in-tree third-party targets (e.g. fetched Jolt,
  manifold) and absolute static archives must match a manifest entry.
- Each notices file starts with the **engine's MIT license (D1)**: the manifest's `engine` entry
  names it, and the text is read from the root `LICENSE` (`--check` fails without it). Then a
  summary with
  COPYLEFT/REVIEW flags, the corresponding-source section for copyleft libraries (Debian source
  package and version), the full license texts, an appendix with the referenced
  `/usr/share/common-licenses` files, the system libraries it relies on but does not ship, and an
  UNRESOLVED list that must be empty.
- `--check` fails on anything unmatched or without a license text, and on a bundled library in
  the manifest's `forbidden` list (`libmpg123`, `libmp3lame`). `--verify-dir DIR --notices F`
  fails if a `.so` in `DIR` is not listed in `F`. With `--manifest`, it also fails if a file there,
  or a `DT_NEEDED` entry of any ELF file in that folder, is forbidden. `dve_package_game` and the
  install-tree test use the latter.
- For a library DVE builds itself (libsndfile), the license texts come from its source tree
  (`COPYING`, `src/ALAC/LICENSE`, `src/GSM610/COPYRIGHT`, `src/G72x/README.original`), and the
  corresponding-source line names the tarball, hash, patches and options instead of a Debian
  package.

### libsndfile without MP3

Debian's `libsndfile1` links MP3 support, which pulled `libmpg123` (LGPL-2.1) and `libmp3lame`
(LGPL-2+; Debian marks `libmp3lame/fft.c` GPL-1+) into every package and game folder. DVE never
needs MP3 at run time. It only opens files for reading (`src/audio/sndfile_decoder.cpp`:
`sf_open(SFM_READ)`), and games load WAV, FLAC, Ogg Vorbis and Ogg Opus samples. MP3 import is an
authoring feature served by the optional FFmpeg CLI path (`DVE_ENABLE_FFMPEG_CLI_IMPORT`).

`DVE_FETCH_SNDFILE` (default `ON`, also set by `linux-gcc-player-release`;
`cmake/DveSndFile.cmake`) therefore builds libsndfile itself, once at configure time, in
`<build>/_deps/sndfile`:

- **Source.** `https://github.com/libsndfile/libsndfile/archive/refs/tags/1.2.2.tar.gz`, SHA256
  `ffe12ef8add3eaca876f04087734e6e8e029350082f3251f565fa9da55b52121`. This is byte-identical to
  Debian's `libsndfile_1.2.2.orig.tar.gz`, and snapshot.debian.org is the fallback URL.
  `DVE_SNDFILE_ARCHIVE=<file>` uses a local copy (offline builds).
- **Patches.** Debian `1.2.2-2+deb13u1`'s patch series is applied on top, vendored unchanged in
  `third_party/libsndfile/patches` (see its README). Upstream has not released the security fixes
  (CVE-2022-33065, CVE-2024-50612), so plain upstream 1.2.2 would be less safe than the Debian
  library it replaces.
- **Options.** `BUILD_SHARED_LIBS=ON`, `ENABLE_MPEG=OFF`, `ENABLE_EXTERNAL_LIBS=ON` (FLAC, Ogg,
  Vorbis, Opus from the system `-dev` packages; they are bundled as before), no programs, examples,
  tests, CPack, pkg-config or man pages. ALSA, Speex, SQLite, mpg123 and lame are not even looked
  for. Configure checks the result with `readelf`: it must link FLAC, Ogg, Vorbis(enc) and Opus, and
  it fails if it links an MPEG library.
- **Shared, not static.** As a separate `lib/dve/libsndfile.so.1`, users can replace the library
  (LGPL-2.1 §6), and we only owe its corresponding source: the tarball, the patches and the options
  above. The generated notices name all three in their "Corresponding source" section. A static
  libsndfile would also oblige us to let users relink the player (ship its object files or source).
- **Only the codecs we use.** libsndfile 1.2.2 cannot disable its external codecs one by one:
  `ENABLE_EXTERNAL_LIBS` is FLAC + Vorbis + Opus together, and `libvorbisenc` comes with Vorbis.
  The built-in formats (WAV, AIFF, ALAC, GSM 6.10, G.72x, …) need no extra libraries. Opus is the
  largest bundled file (Debian's `libopus` is 3.5 MB). It stays because Ogg Opus samples are read at
  run time.
- **Fallback.** With `DVE_FETCH_SNDFILE=OFF`, or when the download, a codec `-dev` package, a
  patch or the build fails (configure warns), the system libsndfile is linked but added to the
  system-library excludes. It is then not bundled (neither are its codecs), and a game needs the
  distribution's `libsndfile1`. Either way no MP3 library is shipped.
- **Other paths to libsndfile.** Only `dve_audio_synth` links libsndfile (SDL3, RtMidi and Steam
  Audio do not). The system `libpulse` also links `libsndfile.so.1`. Because the player uses a
  `DT_RPATH`, a PulseAudio client loaded into the game process picks up the bundled copy, which is
  ABI-identical (same 1.2.2 / `libsndfile.so.1.0.37`).
- **DEB packages** keep depending on the distribution's `libsndfile1` (they never bundle).

`AudioImportCapabilities::sndfileMpeg` reports whether the loaded libsndfile can decode MPEG.
It is false with DVE's build.

### Third-party licenses and what to review

Found on this box (Debian packages) and in the fetched sources. **This is not legal advice**;
the flagged items need a decision before a public release.

| Library | License | Shipped in | Note |
|---|---|---|---|
| SDL3, SDL3_ttf | Zlib (+ permissive sub-licenses) | Runtime (dynamic with `linux-gcc-release`), editor | The fetched SDL `LICENSE.txt` omits the sub-licenses listed in Debian's copyright. |
| libsndfile (DVE's build, 1.2.2 + Debian patches, no MPEG) | **LGPL-2.1+** (built-in ALAC Apache-2.0, GSM 6.10 permissive, G.72x public domain) | Runtime, editor, tools | Shipping requires the corresponding source or a written offer, and the right to relink (satisfied by shipping it as a separate `.so`). See [libsndfile without MP3](#libsndfile-without-mp3). |
| libmpg123, libmp3lame | LGPL-2.1 / LGPL-2+ (lame's `fft.c` GPL-1+ per Debian) | **never** (forbidden) | They were only there because Debian's libsndfile links MP3 support. The notices check, `--verify-dir --manifest`, `dve_package_game` and the package tests fail if either is shipped. |
| libFLAC | BSD-3 (the `flac` tools are GPL) | via libsndfile | |
| libvorbis, libogg, libopus | BSD-3 | via libsndfile | |
| Lua 5.4 | MIT | Lua presets | |
| RtMidi | MIT-style | editor | |
| libjack (JACK1 0.126) | **LGPL-2.1+** (rest of JACK GPL-2+) | not bundled by default | Needs Berkeley DB. `DVE_INSTALL_BUNDLE_JACK=ON` bundles it. |
| Berkeley DB 5.3 (`libdb-5.3`) | **Sleepycat** | not bundled by default | Clause 3 requires offering the source of the DB *and of the software that uses it*: effectively strong copyleft. The Phase 3 editor TGZ bundled it; it is excluded now. |
| libjpeg-turbo | IJG / BSD-3 / Zlib | static in tools/editor | |
| libpng | libpng-2.0 | system library, not shipped | |
| Jolt Physics, Box2D | MIT | static | |
| Box3D | taken from the fetched `LICENSE` | static, only if enabled | **Review**: not built in the verified presets, so its license was not checked. |
| manifold | Apache-2.0 | static | No `NOTICE` file. |
| Steam Audio | Apache-2.0 (Valve, 2024) | only with `DVE_STEAM_AUDIO_ROOT` | **Review**: not present on this box; the text comes from the SDK folder, which may carry more third-party notices. |
| DASHR, BS-Cloth, Fluoddity3D, Gabor fields, Mantaflow, YASPS, Slug adaptations | MIT-0, Apache-2.0, MIT, MIT, Apache-2.0, MIT, MIT/Apache (credit required) | compiled into `dve_core` | Listed for every component, conservatively. |

The engine itself is **MIT-licensed (D1)**: the notices, every CPack package and every
`dve_package_game` folder carry its text. MIT does not change the terms of the libraries above: a
shipped game must still meet their conditions (for example the LGPL corresponding-source
obligation). The adapted code bases (last row) are permissive (MIT-0, MIT, Apache-2.0, MIT OR
Apache-2.0) and compatible with distributing the engine under MIT, provided their notices are kept;
the Apache-2.0 ones (BS-Cloth, Mantaflow) have no upstream `NOTICE` file, and Slug's reference
README additionally asks for credit in distributed software, which its notices section provides.
Their files are not relicensed. A shipped game can carry its own `LICENSE` in the project folder.

### Tests

| Test | What it checks |
|---|---|
| `dve_editor_scene_export_tests` | Exporter mapping, warnings, `--strict`, byte-identical re-export, stale file removal and bad output names; `.dmesh` copy and polygon load (collision and visual-only), the 3D text and Gabor bakes, components (64-bit ints, escaped strings, floats, vectors) and attachments that follow a falling parent. |
| `dve_game_scene_loader_tests` | Also the `extensions` round trip and its validation errors, the attach policy, and (Jolt builds) that an attached child does not push its parent. |
| `dve_player_runtime_tests` | Also that the CPU renderer draws polygon objects. |
| `dve_player_export_scene` | Generates an editor project (ground, dynamic cart with a component and a tag, attached `.dmesh` crate, 3D text attached to the crate, Gabor cloud), exports it with `dve_export_scene --strict`, and runs `dve_player --headless --hash` on it: 5 objects, 2 attachments, the attached objects keep their offsets while the cart falls, Lua sees the component and tag, and the frame hash matches `tests/data/player_golden/expected_export_hashes.txt` (keyed by compiler, Lua and physics backend; unknown keys print the hash to add). |
| `dve_third_party_notices_self_test` | The generator's matching and parsing on synthetic inputs, including the engine's MIT header and a missing `LICENSE`. |
| `dve_third_party_notices_check_<Component>` | `--check` on the real inputs: every linked or bundled third-party library has an entry and a license text, and the engine's `LICENSE` is found. |
| `dve_package_game_test` | Packages `player_sample` with `--tgz --verify`, extracts the archive to a temporary folder, and runs `./Player_Sample --frames 30 --hash` from another directory with no `LD_LIBRARY_PATH`: the pak is found next to the executable, 4 objects load, and the hash equals the build-tree player's and the golden hash. Then it packages a copy of `examples/editor_demo_project` (only an editor scene) and checks the export and the `.autosave` exclusion. Both check RPATH, `ldd`, the notices coverage, `DVE-LICENSE.txt` (the engine's MIT license, also recorded in `build-info.json`) and that no editor-only files or sample maps are shipped. |
| `dve_audio_sndfile_format_tests` | WAV (native and libsndfile), FLAC (bit-exact), Ogg Vorbis and Ogg Opus fixtures (`tests/data/audio_formats`) decode through libsndfile and `import_audio_file()`. With DVE's libsndfile, `sndfileMpeg` is false, libsndfile rejects the MP3 fixture, MP3 still imports through FFmpeg when that is built, and neither `libmpg123` nor `libmp3lame` is loaded in the process (`dl_iterate_phdr`). |
| `dve_install_tree_test`, `dve_package_game_test` (MP3) | `tests/cmake/dve_no_mpeg_check.cmake` on the install prefix and on both game folders: no `libmpg123*`/`libmp3lame*` file, no such `DT_NEEDED` in any shipped ELF (`readelf -d`), none in `ldd` of the shipped executable (not even from the system, with DVE's libsndfile), the shipped `libsndfile.so.1` is DVE's build, and the notices list no MP3 library. |
| `dve_install_tree_test`, `dve_package_consumer_test` | Also check the installed notices (which must state the engine's MIT license), `share/doc/dve-<group>/LICENSE` and `copyright` for every installed group, and build a game package through the installed `dve_add_game_package()`. |

### Windows

Windows 10 or later, x64, MSVC (Visual Studio 2022 or newer). CI builds and tests this on
`windows-latest` (job `windows-msvc`, blocking).

```bat
vcpkg install libpng:x64-windows libjpeg-turbo:x64-windows
cmake --preset windows-msvc-player-release -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build out/build/windows-msvc-player-release --config Release
ctest --test-dir out/build/windows-msvc-player-release -C Release
cmake --build out/build/windows-msvc-player-release --config Release --target dve_sample_game_package
```

**Preset.** `windows-msvc-player-release` uses the newest installed Visual Studio generator
(multi-config, so `--config Release` / `-C Release`), `BUILD_SHARED_LIBS=OFF`, and fetches the same
pinned sources as `linux-gcc-player-release`: static SDL 3.4.12 (`DVE_FETCH_SDL3`), Jolt 5.6.0
(`DVE_FETCH_JOLT`, built with the DLL C runtime `/MD` like the rest) and Lua 5.4.9
(`DVE_FETCH_LUA`, `cmake/DveLua.cmake`: the official `lua-5.4.9.tar.gz`, SHA256-pinned, built as
a static library; vcpkg's `lua` port is 5.5, which the scripts are not written for). libpng and
libjpeg-turbo (and zlib) come from vcpkg as DLLs; only the tools and the editor use them.
Python 3 (`python.exe` is accepted) is needed for the notices and `dve_package_game`.
The preset sets `VCPKG_APPLOCAL_DEPS=OFF`: vcpkg's per-target copy of its DLLs into the shared
output folder fails with sharing violations when MSBuild builds projects in parallel. Instead,
CTest prepends vcpkg's `bin` to `PATH` for every test, `dve_package_game --tool-path` does the same
for `dve_pack`/`dve_export_scene`, and `cmake --install` copies the DLLs. To run a tool straight
from the build folder, put `<vcpkg_installed>/x64-windows/bin` on `PATH`.

**Game folder.** `dve_package_game` recognises a Windows runtime prefix by `bin/dve_player.exe`.
The folder (and the zip, `<Game>-<version>-windows-x86_64.zip`, sorted entries with fixed
timestamps) contains:

```
Player_Sample/
  Player_Sample.exe        dve_player.exe renamed (SDL3, Lua and Jolt are linked in)
  game.dvepak
  MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll   Visual C++ runtime (see below)
  THIRD_PARTY_NOTICES.txt  the Runtime notices
  DVE-LICENSE.txt          the engine's MIT license
  build-info.json
```

- **DLLs next to the `.exe`.** Windows searches the executable's folder first, so no `PATH`
  change or manifest is needed. `dve_package_game` copies exactly the DLLs that the `.exe`
  (recursively, through each DLL's import and delay-import tables) imports from `<prefix>/bin`;
  everything else must be a Windows system DLL. Nothing is stripped (the PDBs are never
  installed).
- **Visual C++ runtime: shipped app-locally (conservative choice).** The executables need
  `MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`, which a clean Windows install
  does not always have. `cmake/DveInstall.cmake` installs them next to the executables with
  CMake's `InstallRequiredSystemLibraries` (`DVE_INSTALL_MSVC_RUNTIME=ON`), so a game folder
  runs without the Visual C++ Redistributable. Microsoft permits redistributing these files
  with an application under the Visual Studio license terms; the notices say so and flag it
  for **review** (`third_party/notices/MSVC_RUNTIME_NOTICE.txt`). Set
  `-DDVE_INSTALL_MSVC_RUNTIME=OFF` to require the Redistributable instead. The Universal CRT
  (`ucrtbase.dll`, `api-ms-win-crt-*`) is part of Windows 10 and is not shipped, so Windows 7/8
  are not supported.
- **Install and CPack.** `install(RUNTIME_DEPENDENCY_SET)` now also runs on Windows: the
  non-system DLLs of each executable are installed to `bin/` by the `*Deps` components (vcpkg's
  `bin/` and `DVE_RUNTIME_DLL_DIRECTORIES` are searched; `C:\Windows` and the API sets are
  excluded). `cpack -G ZIP` packages that tree (built in CI, not uploaded).
- **No audio decoding libraries.** libsndfile is not built on Windows (vcpkg's port pulls in
  mpg123/mp3lame, and `DVE_FETCH_SNDFILE` is Linux-only), so a Windows game reads WAV samples
  only; FLAC/Ogg/Opus samples are not available there yet.
- **Notices and the forbidden-library check.** On Windows, `tools/generate_third_party_notices.py`
  reads PE import tables instead of `readelf`/`ldd`: a DLL found next to the program or in the
  vcpkg/extra search folders is bundled (and must match a manifest entry: libpng, libjpeg-turbo,
  zlib, the Visual C++ runtime with its own entry), a DLL in `C:\Windows\System32` or an
  `api-ms-win-*`/`ext-ms-*` API set is a system library, and anything else is UNRESOLVED (an
  error). vcpkg license texts come from `<vcpkg_installed>/<triplet>/share/<port>/copyright`, Lua's
  from the notice in `lua.h`. The manifest's `forbidden` list also matches
  `mpg123*.dll`/`libmpg123*.dll`/`mp3lame*.dll`/`libmp3lame*.dll` (any case): `--check`,
  `--verify-dir` (file names and PE imports), `dve_package_game` and the Windows package test
  fail on them.

**Tests on Windows.** `ctest -C Release` runs the full suite (279 tests in CI, 2 of them skipped; the Linux player preset registers 284). New:
`dve_package_game_windows_test` packages the sample with `--build-dir --zip --verify`, extracts the
zip, checks the files, `DVE-LICENSE.txt`, `build-info.json`, the notices coverage
(`--verify-dir`), that no MP3 DLL is shipped or imported, and runs `Player_Sample.exe --frames 30
--hash` from another folder with `PATH` reduced to the Windows folders: 4 objects, the pak next to
the executable, and the same frame hash as the build-tree `dve_player`. Skipped or not built on
Windows, with the reason:

| Test | Why |
|---|---|
| `dve_live_editor_mcp_tests`, `dve_live_editor_mcp_core_tests` | Reported as **Skipped** (exit code 77): the live editor's private IPC is a Unix domain socket, not implemented on Windows. |
| `dve_install_tree_test`, `dve_package_consumer_test`, `dve_package_game_test`, `dve_cpack_test` | Not registered: they check ELF details (RPATH, `readelf`, `ldd`, `dpkg-deb`). The Windows equivalents are `dve_package_game_windows_test` and the CI `cpack -G ZIP` step. |
| `dve_native_editor_smoke`, `dve_x11_header_compat_tests` | Not built: they need the X11 native editor (`dve_native_editor_x11`), which is Linux-only. |

Partial on Windows: `dve_audio_sndfile_format_tests` checks WAV only (no libsndfile, see above).
The synth polyphony deadline check is not in CTest on any platform (#40).

Windows variants instead of skips: `dve_udp_multiprocess_tests` starts itself as the client with
`CreateProcess` (Linux uses `fork`); `dve_player_save_load` and `dve_player_runtime_tests` check the
`%APPDATA%` save folder; `dve_ai_assistant_tests` checks that a named validation task is refused,
not launched (named-task execution is not implemented on Windows); `dve_player_default_pak` and
`dve_player_smoke` run headless with the pak next to the `.exe` (the player now uses
`SDL_GetBasePath` there), and the `MSVC-19-lua` golden hash equals GCC's.

### Not done yet

- NSIS installers (configured, not tested); macOS.
- A legal review of the flagged libraries, including how to provide the
  LGPL corresponding source (a pointer to snapshot.debian.org may not be enough).
- Exporter: 3D text and Gabor volumes are baked approximations (see above), not the editor's
  Slug text and volumetric rendering. Mixed voxel sizes still need the GPU renderer to draw in
  the player (the CPU path skips objects whose voxel size differs from the first one).
