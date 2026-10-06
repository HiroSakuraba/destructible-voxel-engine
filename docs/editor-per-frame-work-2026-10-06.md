# Editor per-frame work and idle redraws

Baseline: main commit `7785b0b`.

## Camera targets in `update()`

`NativeEditorController::update()` refreshed a camera-director target for every scene object
on every frame: two quaternion rotations, two normalizations and a `std::map` write per
object. Rigs only read the targets named by their `followTarget` / `lookAtTarget`, so it now
refreshes just those. A named object that no longer exists is removed as a target; before,
its last position stayed in the director, so a rig kept framing a deleted object.

| Scene | `update()` median, main | This change |
|---|---:|---:|
| demo (4 objects) | 0.6 us | 0.4 us |
| demo + 2,000 objects | 189 us | 0.4 us |
| demo + 10,000 objects | 1,450 us | 0.4 us |

Release `-O2`, null canvas, same machine.

## Window title

Both editors set the window title every frame. Each call rewrites the window's title
properties (`SDL_SetWindowTitle`, `XStoreName`) and makes the window manager repaint the
title bar, up to 125 times a second. `SdlApplicationHost::set_window_title` now skips a title
equal to the last one it applied (reset when the window is created or destroyed), and the
X11 editor keeps the last title it stored.

## Idle redraws

The desktop editor redrew at up to 125 fps whether or not anything changed. It now keeps
that rate while input arrives (and for one second after) or while
`NativeEditorController::animating()` is true, and otherwise waits up to 33 ms between
frames, about 30 fps. Input still wakes the wait immediately, so typing, clicking and
dragging respond as before; only frames nobody is changing are drawn less often.

`animating()` is true during a Play/Simulate session, sprite level or 2D rig playback, while
the sprite authoring panel is open (timeline and palette previews), during camera blends,
while fly-navigation keys are held, while a synth voice sounds and while thumbnails are
being generated. Anything not on that list (status countdowns, background task progress,
AI replies, MIDI notes that start no voice) still updates, at about 30 fps when idle.

The X11 editor's loop sleeps without waking on input, so its rate is unchanged.

## Validation

- New `dve_editor_frame_work_tests`: an orbit rig following an object tracks its moves and
  falls back to its authored target once the object is deleted; `update()` with 10,004
  objects stays under 400 us (0.4 us here); `animating()` is false when idle, true while a
  synth voice sounds and during Simulate, and false again afterwards.
- `dve_sdl_application_host_tests`: ten identical titles reach SDL once (the creation title
  is not re-sent), and a changed title is sent. The fake SDL counts `SDL_SetWindowTitle`
  calls and gains `SDL_GetWindowTitle`.
- `dve_desktop_editor_contract --smoke` passes.

Full build (Release, SDL3 unavailable): 252 of 253 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
