# Editor autosave and settings that have no effect

Baseline: main commit `de51c33`.

## Autosave

`editor.autosave_minutes` ("Autosave Interval", default 5) was defined, saved and
shown in Settings, but nothing autosaved. It now does.

- After the configured number of minutes with unsaved changes, the editor clones the
  open scene on the UI thread and writes the clone on a dedicated worker with the
  normal transactional scene save. The open document, its path, revision and unsaved-
  changes flag are untouched; File > Save still decides what the scene file holds.
- Recovery copies go to `<project>/.dve/recovery/<scene>/<scene file name>` (the
  scene's own folder, then the working directory, when no project root is configured).
  Each autosave is written to `<scene>.writing` and swapped in only after it fully
  succeeds, so a failed or interrupted autosave never destroys the previous copy.
- A successful File > Save deletes the recovery copy. An autosave that was still
  running when File > Save happened is discarded rather than reinstated.
- When a scene is opened and its recovery copy is newer than the scene file, the
  Console says where it is and the status bar points there.
- Failures are logged to the Console and never interrupt editing.
  `NativeEditorController::autosave_status()` reports in-flight, completed and failed
  autosaves for tests and tools.

Cloning copied voxels one at a time (`set_voxel` per voxel). `clone_voxel_object` now
copies whole bricks with `snapshot_brick`/`replace_brick` (same voxels, same set of
bricks, generations carried over), which is what keeps the UI-thread part of an
autosave short. It also speeds up the other callers: the background diagnostics
service and prefab capture both clone on the UI thread.

| Scene | Clone, main | Clone, this change |
|---|---:|---:|
| demo (24 bricks) | 0.4 ms | 0.1 ms |
| 16 solid 64^3 objects (8,192 bricks) | 507-545 ms | 24 ms |
| 64 solid 64^3 objects (32,768 bricks) | 1,880-2,266 ms | 96 ms |

Equivalence was checked against the per-voxel clone on 20 random objects (including
empty bricks and fully removed bricks): same brick set, same material in every voxel.

File > Save itself still writes on the UI thread; making it asynchronous is a separate
change.

## Settings that have no effect yet

Of the 216 settings, 134 are not read by anything in the engine or editor: 133 are never
referenced outside their definition, and `accessibility.reduced_motion` is copied into
`EditorPreferences::reducedMotion`, which nothing uses. (`editor.autosave_minutes` was
the 135th and is now applied.) Many look intended for the game runtime or for later
editor work.

- `SettingDefinition::applied` is false for those 134, listed in `kNotYetApplied` in
  `src/editor_settings.cpp`.
- The Settings panel shows "no effect yet" in the policy column and explains it in the
  detail pane. The AI settings tool reports `applied`, so the assistant does not
  suggest settings that do nothing.
- `dve_settings_applied_tests` scans `src/`, `apps/` and `include/` and fails when a
  setting marked applied is never read (wire it up or list it), or when a listed
  setting is now read (remove it from the list). Removing `editor.theme` from the list
  makes it fail with that message.

## Validation

- `dve_editor_autosave_tests`: a clean document is never autosaved; nothing happens
  before the interval and a background save starts after it; the copy loads with every
  object, under `.dve/recovery`, with no staging folder left; the document stays dirty
  with its path and revision unchanged; repeated autosaves replace the copy; File > Save
  removes it; a newer copy is reported when the scene is reopened and an older one is
  not.
- `dve_settings_applied_tests` as above (216 settings, 134 not applied yet).

Full build (Release, SDL3 unavailable): 250 of 251 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
