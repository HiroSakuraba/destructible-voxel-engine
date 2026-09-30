# Save games

Players can save and load the game world, including destruction. A save holds every
GameWorld object, the voxels each one has left after damage, the physics bodies, timers and
pools, and the state of the game's Lua scripts. `dve_player` binds this to quicksave and
quickload, keeps save slots in the user's data directory and can resume a save with
`--load`. Scripts can save and load too.

| Piece | Where |
|---|---|
| Container (header, hashes, limits, migrations, atomic publish, `.bak`) | `SaveGameStore`, `dve/v235_foundations.hpp` |
| World snapshot | `GameWorld::capture_save_state` / `restore_save_state` / `state_hash`, `dve/game_world.hpp` |
| World save format (sections, voxel deltas, source checks) | `GameSaveCodec`, `dve/game_save.hpp` |
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
timer the boot scheduled again. A saved timer with no match cannot be rebuilt (a callback is
code, not data), so it is dropped and counted in `GameWorldRestoreReport::timersDropped`. The
same goes for pools, which are matched by id and name.

`GameWorld::state_hash()` is an FNV-1a hash over all of the above. The player stores it in the
save and checks it after restoring, before any script `on_load` runs.

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
- the value each `world.on_save` handler returns (the key defaults to `"main"`).

Values can be nil, booleans, integers, floats, strings, and tables of those nested up to 32
levels deep. Table keys can be strings, numbers or booleans. Tables are written with sorted
keys, so the same state always produces the same bytes. Saving fails with the offending path
if a handler raises an error or returns something that cannot be saved:

- functions, userdata or threads, e.g. `on_save('main') result.cb.f: cannot save a function`;
- a table that contains itself, e.g. `... the table contains itself (cycle)`.

The script state is at most 16 MiB.

On load, the globals are restored first. Then each `world.on_load` handler whose key has saved
data is called with a fresh copy. `save_game` and `load_game` only queue a request: the player
carries it out after the current fixed step, so a load never replaces the Lua state while Lua
code is running.

## Format (schema version 1)

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
| `dve.script` | opaque bytes from the script host (optional). The Lua blob has its own magic and version (`DVLS`, v1). |
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
| Source asset read on load | 2 GiB |

Saving fails if the world is over a limit. Loading refuses a file that declares more than the
reader's limits allow.

## Versions and migrations

`kGameSaveSchemaVersion` is 1. When the format changes:

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

`dve_game_save_tests` covers this with a synthetic version 0. It uses the same payloads under
the pre-release section names `meta`/`world`/`voxels`/`physics`, plus a `format` marker. With no
migration registered, the load is refused. With the `0 -> 1` migration (which renames the
sections), it loads and restores the identical world.

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
  3. restores the save into it and checks `state_hash()` against the saved hash;
  4. runs the scripts' `on_load` handlers, and only then swaps the fresh world in and restores
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
None of this changes the default 30-frame golden hashes.

Save sizes for the sample, with Jolt and Lua:

| When | File | `dve.meta` | `dve.world` | `dve.voxels` | `dve.physics` | `dve.script` |
|---|---|---|---|---|---|---|
| Fresh (tick 1): 4 objects, 3 bodies, deltas with no changed bricks | 1,414 B | 105 | 543 | 104 | 307 | 175 |
| After the tower blast (tick 31): the tower is a delta, its fragment is stored in full | 2,645 B | 105 | 680 | 1,097 | 408 | 175 |

Without Lua (`--no-script`) there is no `dve.script` section. The per-object density table
(256 entries) is run-length encoded. Before that encoding the same saves were 4.4 KB and 6.4 KB.

## Tests

| Test | What it checks |
|---|---|
| `dve_game_save_tests` | Save and load give an identical `state_hash` and byte-identical re-saves, with the reference solver and with Jolt; continuing both worlds keeps them in lock step. The same after destruction (a static tower split into a dynamic fragment, a chipped dynamic crate) plus ticks, with delta and full objects. Loading into a world that has diverged. A changed source asset is rejected. Every truncation, flipped bytes, trailing bytes, a huge declared size, a corrupt slot falling back to `.bak` (and failing when that is not allowed or missing). The synthetic v0 -> v1 migration, a missing migration, a failing one, one that does not advance, a newer schema. Game sections and limits. Lua script state: nested tables, integer vs. float, globals, deterministic bytes, functions, cycles, errors and depth rejected, malformed blobs, and `save_game`/`load_game` with and without a host handler. |
| `dve_game_world_tests` | Also the regression test for fragmenting a *moved* dynamic object (see CHANGELOG). |
| `dve_player_runtime_tests` | In-process: save, then load in a fresh `PlayerApp` gives the same state hash and the same CPU-rendered frame. The Lua blast, then F5, then F9 restore in place (tick count, objects, frame and Lua spinner state). A fresh app loading the quicksave and the original both run 15 more ticks and stay identical. Lua slot save and load. Truncated, flipped and missing saves fail and leave the game running. Slot names, the slug, and `XDG_DATA_HOME` handling. |
| `dve_player_save_load` | CLI, two processes. Process A plays 40 frames: blast, move, F5 at frame 30, Lua slot save at frame 34, saving to `XDG_DATA_HOME`. Process B runs `--load quicksave`: the loaded frame equals the frame A rendered after saving, and after the remaining 9 frames its hash equals A's final hash. Loading a slot by path works. Truncated, garbage, missing and invalid-slot `--load` exit 3 with a message. |

## Not saved

The following are not saved. These rebuild from the boot, not from the save:

- The state of the sub-runtimes: gameplay characters and player controllers, cameras and camera
  sequences, skeletal animation and controllers, ragdolls, deformables, CPU hair, UI.
- Lua closures and upvalues, and Lua timers that the boot does not schedule again (they are
  dropped and counted).
- Changes a script makes after boot to the environment, the HUD or materials.
- Physics solver caches (contacts, warm starting). In the tests, Jolt and the reference solver
  continued identically after a load, but this is not guaranteed for every scene.

Games that need any of this can put it in `world.on_save` or in a game section.
