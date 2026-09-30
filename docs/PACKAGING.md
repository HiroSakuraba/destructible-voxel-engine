# Packaging and runtime content (Phases 1–3)

This is the runtime half of shipping a game: reading content from a `.dvepak` or a loose
project folder and booting a `GameWorld` from it without the editor (Phase 1), and the
`dve_player` executable that runs it (Phase 2, [below](#the-player-dve_player)), and installing
and packaging the engine (Phase 3, [below](#installing-and-packaging-phase-3)). The
`dve_package_game` step that produces a shippable game folder is Phase 4.

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
| `Tools` | `bin/dve_pack`, `bin/dve_cook_*`, `bin/dve_asset_index`, `bin/dve_prefab_tool` | `dve-tools` |
| `ToolsDeps` | `lib/dve/*.so*` for the tools | `dve-tools` (archives only) |
| `Development` | `include/dve/**` (with the generated `version.hpp`, `build_config.hpp`), `lib/libdve_*.a`, `lib/dve/third_party/*.a`, `lib/cmake/dve/` | `dve-dev` |

Only executables that exist in the build are installed; for example, a build without SDL3 has no
`Runtime` or `Editor` component. `dve_export_scene` and `dve_package_game` come in Phase 4.

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

Installed executables get `INSTALL_RPATH $ORIGIN/../lib/dve`. They are linked with
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
- the font stack (freetype, harfbuzz, fontconfig, libpng16, brotli, bz2).

What gets bundled depends on the preset. With `linux-gcc-release`, the Debian `libSDL3.so.0`
itself links X11, Wayland, PulseAudio, PipeWire and sndio directly, so a TGZ built from it needs
those on the target machine. The portable choice for games is `linux-gcc-player-release`
(static SDL 3.4.12, decision D3), which only bundles Lua, RtMidi and libsndfile with its codecs.

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

On Windows, `CPACK_GENERATOR` is `ZIP;NSIS` and the executables' `$<TARGET_RUNTIME_DLLS>` are
copied next to them. This is configured but **untested** (no Windows runner yet).

**No license (D1).** There is no engine `LICENSE`, so no `CPACK_RESOURCE_FILE_LICENSE` is set,
and `cpack` prints a warning for every generator: these packages are for internal use only. The
third-party notices (`THIRD_PARTY_NOTICES.txt`) come with Phase 4.

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
| Plain library file | `libsndfile.so` | Kept as the absolute path. `dve-dev` depends on the owning `-dev` package. |

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
| `dve_cpack_test` (label `slow`) | `cpack -G "TGZ;DEB"`, then checks each archive's listing and each `.deb`'s `dpkg-deb -c` / `-f` (package name, version, `Depends`, no `lib/dve` in the DEBs, no sample maps). It writes a size summary to `install_tests/cpack/summary.txt`. |
