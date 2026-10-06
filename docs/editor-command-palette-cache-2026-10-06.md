# Command palette result cache

While the command palette is open, `command_palette_results` ran on every pointer move
(to find the row under the cursor) and again when drawing each frame. Each call fuzzy-
scored every command, setting, panel, asset and scene object, so the cost grew with
the scene. Baseline: main commit `de51c33`.

## What changed

- Results are cached and reused until an input they derive from changes. The cache key
  covers the query and limit, favorites and recent commands, a new
  `EditorMenuRegistry::revision()`, a new `EditorAssetDatabase::revision()`, the
  advanced-settings toggle, the number of setting definitions, and a hash of the scene
  objects' ids and names (hashing is much cheaper than fuzzy scoring).
- `EditorMenuRegistry::revision()` advances when an action is added, enabled or
  disabled (or its reason changes), checked, or given a new shortcut. After the legacy
  mutable `find()` has exposed a pointer it returns a new value on every call, so
  nothing derived from the registry is ever served stale.
- `EditorAssetDatabase::revision()` advances on every mutating call, including the
  mutable `find()`, conservatively.
- `command_palette_rebuild_count()` exposes how often the results were recomputed, for
  tests.

## Measurements

Demo scene plus N extra named objects, palette open with a two-letter query, warmed
medians, `-O3`. "Rebuild" on this branch is a cache hit (key check plus copying up to
64 results).

| Objects | Palette results, main | This change | Pointer move with palette open, main | This change |
|---:|---:|---:|---:|---:|
| 4 | 262 us | 9 us | 425 us | 20 us |
| 1,004 | 1,094 us | 17 us | 2,515 us | 34 us |
| 5,004 | 4,884 us | 153 us | 9,744 us | 318 us |

A full rebuild still happens when the query changes (each keystroke), as before.

## Validation

`dve_menu_command_center_v220_tests` adds a cache test: repeated pointer moves and
reads do not rebuild; a new object, a renamed object, a different limit and a query
edit each rebuild and show up in the results; favoriting a command and disabling one
are reflected; and the asset database revision advances on mutating calls (including
the mutable `find`) but not on const reads.

Full build (Release, SDL3 unavailable): 248 of 249 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
