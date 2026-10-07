# Viewport navigation settings

This batch connects two editor preferences. Main after PR #67 has 68 of the
original 134 settings connected; this branch has 70 connected and 64 unapplied.
These counts describe wiring, not certification of every original contract.

## Published navigation gestures

`camera.navigation_style` selects these held mouse gestures in the 3D editor viewport:

| Style | Look / fly | Orbit pivot | Pan | Dolly |
| --- | --- | --- | --- | --- |
| DVE | RMB | Alt + LMB | MMB | Alt + RMB |
| Unity inspired | RMB | Alt + LMB | MMB or Ctrl + Alt + LMB | Alt + RMB |
| Unreal inspired | RMB | Alt + LMB | Alt + MMB or MMB | Alt + RMB |
| Blender inspired | RMB (DVE extension) | MMB | Shift + MMB | Ctrl + MMB |

LMB, MMB and RMB mean left, middle and right mouse button. Scroll-wheel zoom and
existing fly-key bindings remain available. Only free look enables held fly keys;
orbit, pan and dolly do not. These are documented familiar subsets, not complete
emulation of another application's controls, camera models or 2D navigation.

The shortcut registry resolves and displays the effective gestures. Navigation
style changes only untouched built-in navigation slots; it does not replace the
active shortcut profile or unrelated shortcuts. Edited slots, explicit unbindings,
and custom/imported complete profiles retain their authored maps. Resetting a
built-in profile restores its eligibility for the selected navigation style.
A newly introduced default does not take a gesture assigned by a custom edit.

Changing style, active shortcut profile, raw-input preference or camera mode clears
held navigation and smoothing history without changing camera pose. Focus loss,
Escape and capture-ending modal/focus transitions also clear held input.

## Raw mouse input and capture

`input.raw_mouse` requests SDL relative capture only during an active 3D viewport
navigation drag. Fractional `xrel`/`yrel` counts travel through the platform bridge
to the same camera actions without rounding, hover picking, or UI-zoom scaling.
Sensitivity and invert-Y still apply. Existing elapsed-time fly integration and
look smoothing remain in use.

During capture SDL system acceleration is disabled and its extra relative speed
scale is neutral. Previous global hint values are restored on release or failed
acquisition. The UI cursor returns to its pre-capture location. Ordinary editor
pointing stays absolute; selecting, painting and gizmo dragging do not capture.

Button release, Escape, focus loss, modal entry, camera-mode/profile changes,
setting changes and bridge/window destruction release capture. An unavailable
SDL relative mode retains ordinary motion and logs a visible fallback once per
gesture, rather than repeatedly retrying each frame. Off uses ordinary absolute
pointer processing; reaching the screen edge can therefore limit a drag.

This is the SDL desktop editor path, not gameplay cursor possession. The legacy
X11 host still uses absolute motion and does not implement this capture API.
Headless/other hosts use the platform contract's explicit unsupported fallback.
Physical device acceleration and driver behavior require hardware validation;
the local SDL tests use the deterministic shim.

## Validation

All 11 targeted local CTest checks passed: viewport navigation, existing audio/navigation,
shortcut registry, settings runtime/registry/reference audit, SDL host, desktop contract
smoke, Play session, editor behavior and viewport clipping. The platform-boundary and
source-manifest checks also passed. The full engine suite was not rerun here.

`dve_settings_viewport_navigation_tests` checks the published bindings and actual
camera actions, authored-profile preservation, live switch/reset behavior,
fractional raw motion, UI zoom, capture release, stale raw events and failed
acquisition fallback. `dve_sdl_application_host_tests` checks actual platform
event conversion, raw hint values/restoration, cursor restoration and focus loss
through the SDL substitute. Existing shortcuts, settings, audio/navigation,
Play session and desktop contract checks cover integration.

References: [SDL relative mode](https://wiki.libsdl.org/SDL3/SDL_SetWindowRelativeMouseMode),
[system acceleration hint](https://wiki.libsdl.org/SDL3/SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE),
[relative speed hint](https://wiki.libsdl.org/SDL3/SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE),
[Unity navigation](https://docs.unity3d.com/Manual/SceneViewNavigation.html),
[Unreal viewport controls](https://dev.epicgames.com/documentation/unreal-engine/viewport-controls-in-unreal-engine),
[Blender navigation](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/navigation.html).
