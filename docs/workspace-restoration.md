# Per-project workspace restoration

`editor.restore_workspace` now controls the native editor's supported workspace
state. SDL desktop and X11 activate it through the shared controller after opening
an explicit project root. Constructors without a project root do not read or write
workspace files in the working directory.

| State | Behavior |
| --- | --- |
| Panels | Restore known panel visibility flags; hierarchy and inspector visibility change the actual fixed layout. |
| Active tab | Restore Problems, Tasks, Console, Profiler, Assets or Assistant in the bottom panel. |
| Viewport | Restore position, target, up vector, perspective/orthographic projection and orthographic height. Lens, clip distances, navigation mode and display options remain owned by preferences or authored camera rigs. During Play, saving captures the pre-Play editor camera. |
| Asset browser | Restore an existing project-local selected asset by relative path, resolving its current index ID. If the index is absent, resolve after a background scan; a new user selection takes precedence. |
| Open asset | Reopen a saved native `.dvesprite` in the Sprite Editor only when its image and optional palette remain within the project. Unsaved sprite/palette edits are excluded. Script, plugin, import and other authoring routes are never dispatched automatically. |

The record is `<project-root>/.dve/user/workspace.txt`, excluded from Git. Version
1 uses a bounded, line-oriented format with quoted relative asset paths. Unknown
keys and invalid entries are ignored individually; invalid headers/versions or
records over 32 KiB leave defaults intact and log a warning. Camera values must be
finite, bounded and nondegenerate. References are checked after canonical/symlink
resolution and must name regular files within the project. Automatic sprite
reopening also bounds document/dependency sizes and authored image dimensions.
Missing or unsupported sprite documents are skipped with a Console warning.

State is loaded once per project activation, after startup settings and the asset
index. Reconfiguring the assistant for the same project does not reset UI state.
Accepted quit saves once; canceled quit does not write. Switching project roots
saves the old workspace and starts the new project's defaults before applying its
record. Hosts can request an explicit checkpoint with `save_workspace_state()`.
No workspace serialization or filesystem writes run in the frame loop. Replacement
uses a temporary file and an OS replacement operation without deleting the previous
record first; failed writes are reported while quit still succeeds.

Turning the setting off takes effect for subsequent save/activation operations:
start from defaults, ignore the stored record and leave it unchanged. It does not
rearrange an already-open project. Re-enabling permits the next project activation
to restore the retained record. User/Project settings load before activation and
Session overrides retain their usual precedence.

The native layout has no arbitrary docking or movable-panel model, so positions
are not invented or serialized. Other authoring panels and their nested tabs are
not restored. Workspace state never stores scene contents, undo history, recovery
snapshots, credentials, plugin state, or executable asset contents. Existing open
dirty sprite documents are not replaced during project reconfiguration. This is
UI restoration on normal close, not crash recovery or unsaved-document recovery.

## Validation

`dve_settings_workspace_restore_tests` exercises record replacement, version and
size limits, invalid fields, camera validation, project path/symlink containment,
restart restoration, layout application, asset and clean sprite reopening,
disabled-mode record preservation, project isolation, same-project reconfiguration,
canceled/confirmed quit, deleted assets and non-execution of script references.
Existing settings, editor, Play-session, autosave, navigation and desktop contract
checks cover integration. All nine targeted CTest checks passed locally, along
with the platform-boundary and source-manifest checks. The full engine suite was
not rerun. SDL contract smoke uses the deterministic SDL shim;
interactive SDL/X11 hardware acceptance and Windows replacement remain CI/manual
checks.
