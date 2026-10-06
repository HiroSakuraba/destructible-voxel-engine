# Editor frame-pacing floor and menu fallback

Two follow-ups to the editor performance change in PR #42. Baseline: main commit
`3232fcf` (PR #43).

## Frame pacing floor

PR #42 replaced the unconditional 8 ms post-frame sleep with an 8 ms frame budget whose
remainder is spent in an input-interruptible `SDL_WaitEventTimeout`. That removed the
sleep from click latency, but every input event ends the wait, and the editor's SDL
renderer is created without vsync. During a mouse drag (500-1000 Hz on many mice)
frames therefore started as fast as events arrived, where the old loop capped at
about 125 fps.

- `dve/platform/frame_pacing.hpp` holds the pacing rule: wait out the rest of the
  budget interruptibly (unchanged), then, if the wait ended early, sleep until at least
  `minimumIntervalSeconds` (4 ms) after the frame started. Idle pacing is unchanged,
  over-budget frames still never sleep, and an input wake adds at most 4 ms minus the
  frame time, against the 8 ms the pre-#42 loop always added.
- Waits are whole milliseconds and are skipped below 1 ms (as before), so the floor is
  effectively 3-4 ms: at most roughly 250-330 frames per second under an input flood.
- The `render.vsync` setting exists but is not applied to the SDL canvas; that is a
  separate behaviour change and is left alone here.

`dve_frame_pacing_tests` checks the wait arithmetic and simulates the loop with a fake
clock: idle frames are paced at 7.5 ms; an event arriving 0.1 ms into every wait gives
3.6 ms per frame with the floor and under 1 ms without it; 12 ms frames never sleep.
The desktop editor loop itself runs in `dve_desktop_editor_contract` against the SDL
shim. Native SDL3 and display hardware were unavailable, so real input-to-display
latency and native wake timing remain unmeasured.

## Menu fallback after a mutable pointer escapes

The legacy mutable `EditorMenuRegistry::find()` switches a registry to uncached reads
for good, because the caller can edit the returned pointer at any time. In that mode
`menu()` rebuilt the ordered index of every menu in both visibility modes on every
call, so the fallback was several times slower than the code PR #42 replaced.

- `menu()` in that mode now orders only the requested menu, through the same
  `ordered_menu_indices` helper the cache is built from, so cached and uncached menus
  are ordered identically by construction.
- The mutable `find()` is marked `[[deprecated]]`, pointing to the const overload for
  reads and `set_enabled` / `set_checked` / `set_shortcut` for changes. No production
  code called it. Twenty-eight read-only test lookups in fourteen test files now use
  the const overload; previously each such call quietly disabled caching for the rest
  of that test. The test that deliberately exercises the legacy pointer suppresses the
  warning locally.

Full menubar (8 menus, 206 actions, default registry), warmed, 200 calls, GCC 13.3.0
`-O2`, same environment, medians of several runs:

| Registry state | Before PR #42 | Main | This change |
|---|---:|---:|---:|
| Cached | 89 us | 24 us | 24 us |
| After a mutable pointer escaped | 87 us | 820 us | 62 us |

`test_menu_command_center_v220` now also checks that edits made through an escaped
pointer after warming change menu visibility and order, and that uncached menus match
cached menus exactly for every default menu in both modes.

## Validation

Full build (Release, SDL3 unavailable) with no errors and no deprecation warnings.
248 of 249 CTest tests passed; `dve_cpack_test` fails because `dve_desktop_editor`
is not built without SDL3, as on main in this environment.
