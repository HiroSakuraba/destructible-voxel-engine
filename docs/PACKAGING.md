# Packaging and runtime content (Phase 1)

This is the runtime half of shipping a game: reading content from a `.dvepak` or a loose
project folder and booting a `GameWorld` from it without the editor. The player executable,
install/CPack and the `dve_package_game` step are later phases.

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
- An object with `generateCollision=false` becomes a marker object. This matches the editor
  play session, because `GameWorld` has no visual-only voxel object yet.
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
