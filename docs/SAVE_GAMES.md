# Save games

Players can save and load the game world, including destruction. A save holds every
GameWorld object, the voxels each one has left after damage, the physics bodies, timers and
pools, the world's sub-runtimes (characters and players, triggers, cameras, skeletal
animation, animation controllers, control rigs, ragdolls and CPU hair), and the state of the
game's Lua scripts (globals, `on_save` data, named timers, and the render environment, HUD and
material changes scripts made). `dve_player` binds this to quicksave and
quickload, keeps save slots in the user's data directory and can resume a save with
`--load`. Scripts can save and load too.

| Piece | Where |
|---|---|
| Container (header, hashes, limits, migrations, atomic publish, `.bak`) | `SaveGameStore`, `dve/v235_foundations.hpp` |
| World snapshot | `GameWorld::capture_save_state` / `restore_save_state` / `state_hash`, `dve/game_world.hpp` |
| World save format (sections, voxel deltas, source checks) | `GameSaveCodec`, `dve/game_save.hpp` |
| Sub-runtime snapshot (v2) | `capture_game_runtime_state` / `restore_game_runtime_state`, `dve/game_save.hpp`; each runtime's `capture_save_state` / `restore_save_state` |
| Script state | `GameScriptHost::save_state` / `load_state`, `world.on_save` / `world.on_load`, `dve/game_script.hpp` |
| Player integration | `PlayerApp::save_game` / `load_game`, `dve/player/player_app.hpp`; `dve_player --load`, `--save-dir` |

## Design: built on the v2.35 `SaveGameStore`

v2.35 added `SaveGameStore`, a versioned, sectioned container (`DVESAVE1`). It has FNV-1a
hashes per section and for the whole document, per-version migrations, atomic publish
(`<slot>.tmp` is renamed into place) and `.bak` rotation with recovery. Nothing used it: its
only caller was its own test. That covers most of what a player save needs, so save games use
it instead of adding a second container format. The world format is a set of sections inside
a `DVESAVE1` document.

To make it safe for files players will copy around, `SaveGameStore` was hardened. The byte
layout is unchanged:

- Every length field is checked against the configured limits and against the bytes actually
  left in the file *before* anything is allocated. Previously a truncated or hostile file could
  make it allocate up to 1 GiB.
- Sections must be sorted and unique, trailing bytes are rejected, and every error names what
  is wrong (for example `save file is truncated (section 4 needs 4454 bytes, 1249 left)`).
- The encoding is explicitly little endian. It used to be host order, which is the same bytes
  on every platform the engine ships on.
- `read()` returns a report: whether the primary or the `.bak` was used (and why the primary was
  rejected), the stored schema version, how many migrations ran and the file size.
- `migrate()` rejects a save from a newer engine, a missing migration step, and a step that
  does not advance the version by exactly one.
- `write_atomic()` creates the slot's folder and reports the bytes written.

`encode_save_game_document` / `decode_save_game_document` expose the container on its own
(no migrations), so tests and tools can build documents directly.

## What is saved

`GameWorld::capture_save_state()` returns a plain-data `GameWorldSaveState`:

- **Objects** (sorted by id): id, name, tags, groups, layer, components (with properties),
  enabled, attachment (parent, local transform, socket, inherit flags), authored transform,
  voxel size, geometry kind (marker, voxel or polygon), flags (dynamic, structural, visual-only,
  has body), pool membership and the source asset (see below).
- **Voxels** of every voxel object: each brick in storage order (key, generation, 512
  material ids), the `VoxelObject` id, the render material table and the density table used for
  mass. Brick generations are kept, so `VoxelObject::state_hash` is identical after a load.
- **Physics bodies**: previous and current transform, linear and angular velocity, sleeping
  flag and the local centre of mass, as the backend (Jolt or the reference solver) reports them.
- **World clocks and counters**: the next object, pool and timer ids, the elapsed time, live
  timers (id, fire time, interval) and pools (id, name, capacity, free list).

`restore_save_state()` first checks and prepares everything, so a bad state changes nothing.
Then it:

1. Removes objects that are not in the save. No destroy events fire, because this is a load,
   not gameplay.
2. Rebuilds every physics body in id order.
3. Replaces the rest by id, so sub-runtime bindings keyed by object id stay valid.
4. Restores timers and pools.

Timers keep their callbacks from the freshly booted world: a saved timer is matched by id to a
timer the boot scheduled again, and the restored timers keep the saved order. A saved timer
with no match has no callback (a callback is code, not data). By default it is dropped and
counted in `GameWorldRestoreReport::timersDropped`. With
`GameWorldRestoreOptions::keepUnboundTimers` it is kept, *unbound*, in its place in the
schedule (it never fires), so that something that knows how to rebuild the callback can attach
it by id with `bind_restored_timer(id, callback)`; `drop_unbound_timers()` then removes the
rest. The player uses this for named Lua timers (below). Pools are matched by id and name.

`GameWorld::state_hash()` is an FNV-1a hash over all of the above. The player stores it in the
save and checks it after restoring, before any script `on_load` runs.

### Sub-runtimes (schema v2)

`capture_game_runtime_state(world)` returns a `GameSaveRuntimeState` with one plain-data
snapshot per runtime; `restore_game_runtime_state(world, state, &report)` applies it to a world
that was just restored. The runtime's *content* (skeletons, clips, controller and rig assets,
ragdoll definitions, grooms, camera rigs and sequences, viewports) is code or asset data that
the game binds again when it boots; the save holds what changes while playing, and restore
matches it to what the boot bound:

| Runtime | Saved | Matched by |
|---|---|---|
| `GameplayRuntime` (`dve.gameplay`) | characters (config, velocity, grounded/support, stance, coyote and jump-buffer timers, input), players (name, local, possessed pawn, input), triggers (desc, occupants, fired), input recordings and playbacks, id counters, fixed tick | replaces the whole runtime; every pawn must exist in the restored world |
| `GameCameraRuntime` (`dve.cameras`) | `serialize_state()`: accessibility settings, per viewport the live rig, director state, sequence time and playing flag, cut generation | viewport, rig, state and sequence ids from the boot |
| `SkeletalAnimationRuntime` (`dve.animation`) | active clip and time, crossfade target, time and progress, playback speed, root-motion flags and pending delta, the current local pose | object id and skeleton content hash; clips by name |
| `AnimationControllerRuntime` (`dve.animation`) | current state, state time, parameters (typed), pending triggers | object id and controller name; states and parameter types must still match |
| `ControlRigRuntime` (`dve.animation`) | enabled flag and every control value | object id and rig content hash |
| `RagdollRuntime` (`dve.ragdolls`) | state (animated, blending in, simulating, recovering), blend, settle timers, recovery poses and facing; for a physically active ragdoll the pose and object transform at activation plus every body's state | object id; body and joint counts |
| `CpuHairRuntime` (`dve.hair`) | running, visible, gravity, wind, root transforms, root-target overrides, the solver's per-point positions, previous positions and velocities, sleep state, step accumulator and frame | owner id and groom content hash |

Restore never fails the load for a mismatch in animation, ragdolls, hair or cameras: an entry
the fresh boot did not bind (a patched game, or something bound at runtime rather than at boot)
is skipped with a message in `GameSaveRuntimeReport::warnings`, and the player logs it. The
gameplay snapshot is the exception: characters are core game state, so an invalid one (a pawn
that is not in the world) fails the load.

A physically active ragdoll is rebuilt by recreating its bodies and joints from the recorded
activation pose (so the joint frames are the ones the original activation used) and then
setting every body to its saved state. The solver's warm-start data is not saved; in the tests
the reference solver continued identically.

### Script state (Lua)

```lua
world.on_save(function() return { spinner = spinner.save(state), blasts = blasts } end)
world.on_load(function(saved) spinner.load(state, saved.spinner); blasts = saved.blasts end)
world.on_save("inventory", function() return items end)   -- more keys: one handler per key
world.save_game("slot1")      -- true, or nil + error; the default slot is "quicksave"
world.load_game("slot1")
world.save_exists("slot1")
```

`GameScriptHost::save_state()` stores:

- every `world.set_global` and `world.set_global_vector` value;
- the value each `world.on_save` handler returns (the key defaults to `"main"`);
- (v2) the render environment (`world.set_environment_*`), the HUD model (interaction
  prompt, tool wheel and selection), the runtime material overrides
  (`world.set_material_parameter`, `world.set_material_layer_weight`) and the named timers.

Values can be nil, booleans, integers, floats, strings, and tables of those nested up to 32
levels deep. Table keys can be strings, numbers or booleans. Tables are written with sorted
keys, so the same state always produces the same bytes. Saving fails with the offending path
if a handler raises an error or returns something that cannot be saved:

- functions, userdata or threads, e.g. `on_save('main') result.cb.f: cannot save a function`;
- a table that contains itself, e.g. `... the table contains itself (cycle)`.

The script state is at most 16 MiB.

**Named timers.** A Lua timer made with `world.schedule_once(seconds, fn)` holds a closure, so it
cannot be saved: after a load it survives only if the boot schedules it again (with the same
id). Timers that must survive a save use a named handler and plain data instead:

```lua
world.timer_handler("aftershock", function(data, id) ... end)   -- at load time, like on_load
local id = world.schedule_once_named(0.5, "aftershock", { blast = blasts })
world.schedule_repeating_named(2.0, "wave", 3)
world.cancel_timer(id)
```

The data is anything `on_save` could return. It is saved with the timer's id, handler name
and kind, and the handler is looked up by name when the timer fires. On load the player keeps
saved timers that the boot did not schedule again (see *Timers* above); `load_state` re-attaches
each named one whose handler is registered, and the player drops what is left (anonymous
closures) and reports the counts in `PlayerLoadResult::namedTimersRestored` / `timersDropped`.
Scheduling with an unregistered handler name is an error.

On load, the globals are restored first. Then the environment, HUD and material overrides
(a saved environment that no longer validates, or an override for a material or parameter the
game no longer has, is skipped with a log message), then the named timers. Then each `world.on_load` handler whose key has saved
data is called with a fresh copy. `save_game` and `load_game` only queue a request: the player
carries it out after the current fixed step, so a load never replaces the Lua state while Lua
code is running.

## Format (schema version 2)

A `.dvesave` file is a `DVESAVE1` document:

```
"DVESAVE1" | u32 schemaVersion | u64 sequence (= tick count) | u32 sectionCount | u64 documentHash
sectionCount x { u32 nameBytes | u64 payloadBytes | u64 payloadFnv1a | name | payload }
```

Sections are sorted by name. `documentHash` is FNV-1a over each name followed by its payload
hash, so a flipped byte, a renamed section or a reordered section is caught. Only the
informational `sequence` field is outside the hashes. All integers and floats are little
endian. Strings are a `u32` length followed by the bytes. Every count is checked against the
limits and against the bytes left.

| Section | Contents |
|---|---|
| `dve.meta` | engine version, game name and version, scene path, tick count, `worldStateHash`, a free-form info map (the player writes `physics=<backend>`) |
| `dve.world` | next ids, elapsed time, the objects (everything except bricks and bodies; the 256-entry density table is run-length encoded), timers, pools |
| `dve.voxels` | per voxel object: `u64 id`, `u64 voxelObjectId`, then **delta** or **full** |
| `dve.physics` | per body: `u64 id`, previous and current transform, linear and angular velocity, sleeping, local centre of mass |
| `dve.script` | opaque bytes from the script host (optional). The Lua blob has its own magic and version (`DVLS`, v2; v1 blobs still load). |
| `dve.gameplay` | v2, optional: character, player, trigger, recording and playback records, then the id counters and fixed tick |
| `dve.cameras` | v2, optional: the camera runtime's text state |
| `dve.animation` | v2, optional: three present-flagged lists (skeletal instances with their poses, controllers with typed parameters, control rigs) |
| `dve.ragdolls` | v2, optional: per ragdoll the state, blend, recovery poses, activation pose and transform, body states |
| `dve.hair` | v2, optional (read only by `DVE_ENABLE_CPU_HAIR` builds; others ignore it): per owner the solver state arrays |
| *game sections* | anything a game adds through `GameSaveData::gameSections`. Names are 1–64 bytes and must not start with `dve.`. |

**Source assets.** The scene loader and `world.spawn_asset` record each object's source asset
path and the FNV-1a hash of its bytes (`GameWorld::set_object_source`). Fragments split off by
damage inherit the source, marked `derived`. When a save loads, every source is read again
through the game's content (the `.dvepak` or project folder) and checked against the recorded
hash. If the game was patched and an asset changed, the load fails with `source asset '<path>'
does not match the save (the game content changed)`.

**Voxel deltas with a full fallback.** An object that *is* its source asset (not a fragment),
still has the same `VoxelObject` id, and whose bricks still start with the source's bricks in
the same order is stored as a **delta**: the source's brick count, then only the bricks that
differ, each tagged with its storage index. Undamaged scene objects therefore cost a few bytes.
Everything else is stored **full**, every brick. That covers fragments, runtime-spawned objects
and objects whose brick layout no longer matches the source. Each stored brick is raw (512 bytes)
or run-length encoded (`u16` run count, then `(length-1, material)` pairs), whichever is smaller.
Material tables are marked "from source" when they are byte-identical to the source's, and
stored inline otherwise.

Polygon (`.dmesh`) objects must have a source. Their geometry is re-read from it on load.

### Limits

These are the `GameSaveLimits` defaults. They are consistent with the engine's other formats:
content paths are limited to 4 KiB, as in `ContentSource`, and every size is checked before
allocating, as in the pak reader.

| Limit | Default |
|---|---|
| File size | 256 MiB |
| Objects | 65,536 |
| Voxel bricks (all objects) | 2^20 (512 MiB decoded) |
| String (name, tag, path) | 4 KiB |
| Tags, groups, components per object; properties per component; materials per object | 256 each |
| Timers | 65,536 |
| Pools | 4,096 |
| Script state | 16 MiB |
| Game sections | 32 |
| Sub-runtime entries (characters, bones per pose, replay frames, ...) | 2^20 per list |
| Hair points per array | 2^24 |
| Source asset read on load | 2 GiB |

Saving fails if the world is over a limit. Loading refuses a file that declares more than the
reader's limits allow.

## Versions and migrations

`kGameSaveSchemaVersion` is 2. When the format changes:

1. Bump `kGameSaveSchemaVersion`.
2. Register a migration from the old version. A migration receives the `SaveGameDocument`
   (its sections as bytes), rewrites it, and must set `schemaVersion` to the next version.

Every read and decode runs the migrations one step at a time before interpreting the sections.
Games that embed `PlayerApp` can register their own migrations through
`PlayerApp::save_codec().register_migration(...)`. The load fails, with the version in the
message, when:

- the save is from a newer engine;
- a migration step is missing;
- a migration fails (with its own message);
- a migration does not advance the version.

**v1 -> v2.** `GameSaveCodec` registers this step itself (so a game cannot register another
one for version 1). Version 2 only adds optional sections, and the `DVLS` script blob reads
both its versions, so the step checks that the document has the four required v1 sections and
no engine section v1 did not define, then sets the version to 2. A v1 save therefore loads with
its world, voxels, bodies, timers and script state, and every sub-runtime stays as the boot left
it (as it did under v1). Saving again writes v2.

`dve_game_save_tests` covers this with a real v1 document, and with a synthetic version 0
that uses the same payloads under the pre-release section names `meta`/`world`/`voxels`/
`physics`, plus a `format` marker. With no migration registered, v0 is refused. With a game's
`0 -> 1` migration (which renames the sections) plus the engine's `1 -> 2`, it loads and
restores the identical world.

## The player

- **Quicksave and quickload.** These are the reserved actions `quicksave` and `quickload`. They
  default to `key:f5` and `key:f9` unless `game.dvegame` binds them (for example
  `bind.quicksave=key:f5,gamepad:back`). They must be actions, not axes. They fire when the key
  is first pressed, and the save or load runs after the current fixed step. Quicksave writes the
  slot `quicksave`.
- **Save folder.** `$XDG_DATA_HOME/dve/<game>/saves`, where `XDG_DATA_HOME` is used only if it
  is an absolute path, as the XDG spec requires; otherwise `~/.local/share/dve/<game>/saves`.
  On Windows it is `%APPDATA%\dve\<game>\saves`, and on macOS
  `~/Library/Application Support/dve/<game>/saves`. `<game>` is the manifest name as a slug
  (`Player Sample` becomes `player-sample`). Override it with `--save-dir <dir>` or
  `PlayerBootOptions::saveDirectory`. The folder is created on the first save.
- **Slots.** A slot is `<slot>.dvesave` in the save folder. Slot names are 1–64 characters of
  `A-Z a-z 0-9 _ - .` and cannot start with `.`. Scripts can only use slot names. `--load` and
  `PlayerApp::load_game` also accept a path (anything with a `/` or ending in `.dvesave`).
  Writes are atomic, and the previous file is kept as `<slot>.dvesave.bak`. If the primary file
  cannot be read, the load uses the `.bak` and logs why.
- **Loading.** `PlayerApp::load_game`:
  1. reads and checks the file, and checks that the save is for this game (manifest name);
  2. boots a *fresh* world and script host for the saved scene;
  3. restores the save into it (keeping unbound timers) and checks `state_hash()` against the
     saved hash;
  4. restores the sub-runtimes (warnings are logged);
  5. restores the script state: globals, environment, HUD, materials, named timers, then the
     `on_load` handlers;
  6. drops the timers nothing re-attached, and only then swaps the fresh world in and restores
     the tick count.

  If anything fails, the running game is untouched. `dve_player --load <slot|file>` does the
  same at start-up. A bad save exits with code 3, like other content errors.
- **Output for tests.** In deterministic runs `dve_player` prints the following lines, each
  followed by the file path, size, tick and the hash of the frame rendered right after the
  event:
  - `save_dir=<dir>`;
  - `saved=` for every save and `loaded=` for every load during the run;
  - `resumed=...`, `resumed_tick=...` and `resumed_framebuffer_fnv=...` for the frame rendered
    after `--load`, before the first tick.

### The sample game

`tests/data/player_sample` binds F5 and F9. Its script also binds:

- **B** blasts a slab out of the tower, so the top breaks off as a falling fragment;
- **1** and **2** save and load `slot1` through `world.save_game` / `world.load_game`.

The spinner's state lives in Lua locals and goes through `world.on_save` / `world.on_load`.
Each blast also schedules a named `aftershock` timer half a second later, which sets the global
`aftershocks` and raises the exposure; a save made in between fires it after loading.
None of this changes the default 30-frame golden hashes.

Save sizes for the sample, with Jolt and Lua (v2):

| Section | Fresh (F5 at frame 2) | After the blast (F5 at frame 30) |
| --- | ---: | ---: |
| `dve.meta` | 105 B | 105 B |
| `dve.world` | 543 B | 696 B |
| `dve.voxels` | 104 B | 1,097 B |
| `dve.physics` | 307 B | 408 B |
| `dve.script` | 339 B | 386 B |
| `dve.gameplay` | 40 B | 40 B |
| `dve.cameras` | 55 B | 55 B |
| `dve.animation` | 15 B | 15 B |
| `dve.ragdolls` | 4 B | 4 B |
| `dve.hair` | 4 B | 4 B |
| **File** (payloads + headers) | **1,852 B** | **3,146 B** |

Sizes are payload bytes; each section also has a 20-byte header plus its name, and the file a
32-byte header.

The v2 sections for the sample's empty runtimes (no characters, viewports, skeletons, ragdolls
or hair) cost about 270 bytes with their headers; the rest of the growth over v1 (1,414 B fresh and 2,645 B after
the blast) is the script state's environment and HUD, and the pending aftershock timer. Without
Lua (`--no-script`) there is no `dve.script` section. The per-object density table (256
entries) is run-length encoded.

## Tests

| Test | What it checks |
|---|---|
| `dve_game_save_tests` | Save and load give an identical `state_hash` and byte-identical re-saves, with the reference solver and with Jolt; continuing both worlds keeps them in lock step. The same after destruction (a static tower split into a dynamic fragment, a chipped dynamic crate) plus ticks, with delta and full objects. Loading into a world that has diverged. A changed source asset is rejected. Every truncation, flipped bytes, trailing bytes, a huge declared size, a corrupt slot falling back to `.bak` (and failing when that is not allowed or missing). The synthetic v0 -> v1 -> v2 migration, a missing migration, a failing one, one that does not advance, a newer schema. Game sections and limits. Lua script state: nested tables, integer vs. float, globals, deterministic bytes, functions, cycles, errors and depth rejected, malformed blobs, and `save_game`/`load_game` with and without a host handler. **v2:** a world with a possessed character, a boot and a runtime trigger, a recording, an animation controller mid-transition, a crossfading skeleton, a control rig, an active ragdoll, CPU hair in wind and a camera state saves, loads into a fresh boot and re-encodes every sub-runtime section byte for byte; after 30 more ticks characters, animation, cameras and hair are still byte-identical and the ragdoll within 5 cm (0 m with the reference solver). A boot missing a binding gives a warning; a character without a pawn fails; every truncation of each v2 section and an unknown `dve.` section are rejected. A real v1 document migrates and loads; a mislabelled one is refused. Unbound timers keep their place and bind or drop explicitly. Script state v2: environment, HUD, material overrides and named timers (boot-scheduled and runtime-scheduled) restore, a cancelled one does not come back, an anonymous closure is dropped, the same named timers fire in both processes; unsaveable timer data and unknown handler names are errors; a v1 blob still loads. |
| `dve_game_world_tests` | Also the regression test for fragmenting a *moved* dynamic object (see CHANGELOG). |
| `dve_player_runtime_tests` | In-process: save, then load in a fresh `PlayerApp` gives the same state hash and the same CPU-rendered frame. The Lua blast, then F5, then F9 restore in place (tick count, objects, frame and Lua spinner state). The blast's pending named `aftershock` timer is re-bound on load (`namedTimersRestored == 1`, nothing dropped). A fresh app loading the quicksave and the original both run 15 more ticks and stay identical, and the aftershock fires in both with the same environment change. Lua slot save and load. Truncated, flipped and missing saves fail and leave the game running. Slot names, the slug, and `XDG_DATA_HOME` handling. |
| `dve_player_save_load` | CLI, two processes. Process A plays 40 frames: blast, move, F5 at frame 30, Lua slot save at frame 34, saving to `XDG_DATA_HOME`. Process B runs `--load quicksave`: the loaded frame equals the frame A rendered after saving, and after the remaining 9 frames its hash equals A's final hash. Loading a slot by path works. Truncated, garbage, missing and invalid-slot `--load` exit 3 with a message. |

## Not saved

What is still not saved, and why:

- **Lua closures and upvalues**, so timers made with `world.schedule_once` /
  `schedule_repeating` survive a load only if the boot schedules them again. A closure's code
  can be dumped, but its upvalues can be shared with other closures, reference C functions,
  userdata and the host's registry, and a dumped function is tied to the exact Lua build and to
  a script that may have been patched. There is no sound, portable way to rebuild one, so the
  supported path is named timers (handler name + plain data). A load logs how many anonymous
  timers were dropped.
- **Deformables (`DeformableRuntime`).** The runtime is behind `DVE_ENABLE_DEFORMABLE_RUNTIME`,
  which none of the release presets (and so none of the CI legs) build, so a save path could
  not be tested there. It also exposes its soft-body state read-only (`state()`) with no way to
  write it back; it needs the same kind of capture/restore pair hair got, plus a preset that
  builds it. A save from a build with deformables is still readable elsewhere: nothing about
  them is written.
- **UI runtime (`ui::UiRuntime`) widgets, focus and data model.** Scripts cannot reach it (no
  Lua bindings) and the player does not create one; canvases are rebuilt from UI assets. The
  HUD model scripts do drive is saved.
- **Runtime-created content**: master materials and material instances created after boot,
  camera viewports, rigs and sequences added after boot, and skeletons, controllers, rigs,
  ragdolls or grooms bound after boot. The save stores their *state*, not their definitions;
  entries whose definition the fresh boot does not recreate are skipped with a warning.
  Serialising the definitions would make every asset type part of the save format.
- **Hair collision sets and solver settings, ragdoll configs, character telemetry** are content
  or per-step diagnostics and come from the boot.
- **Camera smoothing internals** (damped follow positions, an in-progress blend between rigs,
  active shakes): the director re-converges within a few frames. Saving them would mean
  serialising `CameraDirector`'s private state.
- **Physics solver caches** (contacts, warm starting), for world bodies and ragdoll bodies. In
  the tests, Jolt and the reference solver continued identically after a load, but this is not
  guaranteed for every scene.

Games that need any of this can put it in `world.on_save` or in a game section.
