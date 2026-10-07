# Short presses between simulation ticks

Found in the Clockwork timing review (main `942dba9`); this change is on main `0c0f977`.

## The bug

The game simulation runs at a fixed 60 Hz, while the window runs at the display's rate. At
144 Hz most frames run no tick at all. Both input paths kept only which keys and buttons are
currently held:

- `dve_player`: `PlayerInput` adds a key on key-down and removes it on key-up, and
  `PlayerApp::tick` copies "held or not" into the world before each tick.
- Play-in-editor: `EditorPlaySession::set_action_pressed` stores the current state, and each
  fixed step copies it into the world.

A tap whose key-down and key-up both arrive between two ticks (about 7 ms at 144 Hz, or a
fast click on a slow frame) was therefore never seen by the game: no jump, no fire, no
quicksave.

## The fix

Both paths remember presses since the last tick.

- `PlayerInput` keeps the keys, gamepad buttons and mouse buttons pressed since the last
  `end_tick()`. An action reports pressed while a source is held or was pressed since then.
  `PlayerApp::tick` calls `end_tick()` after the tick's input has been copied into the
  world, so a tap is pressed for exactly one tick. Focus loss clears these presses with the
  held state. Axes still report held state only.
- `EditorPlaySession` keeps the actions pressed since the last fixed step and marks them
  pressed in that step's input even if they were released first, then clears them. A new
  `runtime_action_pressed()` reports what the running world saw, for tests and diagnostics.

A held key behaves exactly as before. The reserved quicksave and quickload keys now also
work with a quick tap.

## Validation

- `dve_player_runtime_tests`: a key tap and a mouse tap released before any tick still make
  the action pressed until `end_tick()`, then not.
- `dve_editor_play_session_tests`: at 144 Hz frames on a 60 Hz step, a jump pressed and
  released before the first step is pressed on that step and not on the next; a held jump
  stays pressed.

Full build (Release, SDL3 unavailable): 257 of 258 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
`dve_player_runtime_tests` passed in a separate `DVE_BUILD_PLAYER=ON` build.
