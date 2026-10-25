# Morphing Flight F1 — implemented first vertical slice

This change introduces `FlyingGame` as a separate prototype gameplay role, rather than treating an aircraft as a racing car. One actor uses the same world object ID and keeps position, orientation, world velocity and angular velocity when a new flight form is requested. Fixed-step updates run at 120 Hz and write `DVE_FLIGHT 1` state into the existing prototype save pipeline.

## Flight mechanics

| Form | Model | Energy |
|---|---|---|
| FLOAT | Near-neutral buoyancy and strong drag relative to wind | Ballast/trim uses battery |
| GLIDER | Wing lift proportional to dynamic pressure, angle-of-attack stall/drag | No motor |
| BIPLANE | Higher wing area and low-speed propeller thrust | Fuel |
| JET | Smaller, fast wing, jet spool and forward thrust | Fuel |
| HOVER | Active rotor gravity compensation and speed/position assistance | Battery |
| BIRD | Lift/drag, optional stamina-limited flap impulse | Stamina |

The F1 aerodynamic coefficients are stylized and editable through the source's six `FlightProfile` records; they are not aerospace-certified. Wind includes uniform airflow plus a finite updraft near world position (50, 280). Force computations use *air-relative* velocity. Morphing blends physical coefficients; mode changes never reset world velocity. Rotation updates are tested against body/wingtip collision proxies before applying them; translation uses three forward sweeps. Crashes and out-of-bounds recover to a checkpoint with a five-second penalty.

## Try it

Run `dve_create_flying /path/to/new-project` (requires the editor CMake target), then `dve_player /path/to/new-project`. Alternatively use **Tools > Gameplay Prototype Kit > Create flying transformation course** in an empty scene, export, and play. Each gate is a broad visible colored opening with a corresponding required flight form; gates are ordered from FLOAT through BIRD. The starter uses CPU-renderable polygon meshes, no downloaded assets. Exported `game.dvegame` carries keyboard/gamepad controls.

Controls: W/S pitch or hover translation, A/D roll or strafe, Q/E yaw, Shift/Ctrl throttle, arrow up/down hover climb, Tab/Backspace cycle modes, 1-6 direct form requests, Space flap in bird mode, B airbrake, C chase/cockpit, X recover, P pause, R restart, F3 diagnostics. State and camera are Z-up with craft-local +Y forward.

## Scope and known constraints

This F1 change delivers an integrated playable controller, basic static wind and updraft, camera, UI, generated level, save hook, and focused tests. It does **not** yet provide guided lessons, wind shear/ridge-lift fields, accurate aerodynamic tables, morphing meshes, articulated control surfaces, per-wing damage, spatial audio or physically simulated engine rotor blades. Float is passive buoyancy, hover consumes battery. Bird stamina recovers over time; energetic bird flight still requires flapping or a real updraft. The collision proxies are deliberately approximate.

### Acceptance / verification

- Compile `dve_flying_game_tests` and run `ctest -R dve_flying_game_tests --output-on-failure`.
- Test scene validation, six-mode changes, fixed-substep result equivalence, pause, restart, exported game boot.
- Hand test keyboard/gamepad controls, camera obstruction, collision recovery, glider sink/stall, biplane/jet fuel and hover battery.
- Run full CI before merging. Headless/reference builds must not require Jolt, an SDL audio device, or outside assets.

Follow-up F2 should tune gliding/stall curves using physics energy audits, author thermal and ridge volumes, and validate soaring/hover in distinct wind environments.
