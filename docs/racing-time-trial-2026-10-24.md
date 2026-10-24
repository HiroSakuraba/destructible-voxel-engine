# Racing time trial starter

This adds a single-player racing example to the shared prototype kit on main. Editor Play and the standalone player use the same runtime. It is independent of the material-preset batch.

## Playable loop

Create the racing starter from Tools > Gameplay Prototype Kit in an empty scene, or generate a complete project with `dve_create_racing NEW_DIRECTORY`. The car, paved circuit, infield barriers, outer walls, four gate stripes/pylons and direction markers are ordinary editable objects. Starter creation preserves the open document and is one undo step. It refuses occupied scenes; project export refuses existing directories.

After a three-second countdown, drive counterclockwise through gates 1, 2, 3 and finish 0. Complete three laps. Only a forward crossing of the next required gate advances the sequence. Gate-plane crossings are swept along the motion segment, with width and height bounds; proximity alone cannot count a checkpoint. The car starts just beyond the finish line, with gate 1 next.

The HUD shows speed in kilometres per hour, lap progress, last lap, total time, session best and the next gate. Pause freezes both world and race clock. Finish freezes the race. Restart resets the attempt and retains session best. Recovery moves the car to the last passed gate, clears velocity and adds five seconds to the current attempt/lap. Falling below the configured kill height recovers automatically. An E/West toggle switches collision-shortened chase and fixed hood cameras.

## Components and units

| Component | Settings |
|---|---|
| `dve.prototype.race_car` | Stable key, top speed (m/s), acceleration/braking (m/s²), lateral grip (1/s), wheelbase (m), steer angle (rad) |
| `dve.prototype.race_rules` | Stable key, target laps, countdown (s), recovery kill Z (m) |
| `dve.prototype.race_gate` | Stable key, contiguous zero-based order, half width/height (m) |
| `dve.prototype.race_state` | Runtime-only versioned save payload |

There must be exactly one enabled car with visible, collision-disabled geometry, one enabled bodyless Rules marker, and 2–128 enabled gates. Gate zero is finish; orders must be unique and contiguous. A racing scene cannot also contain first-person prototype roles/event links. Car/rules tuning and gate dimensions are finite and range-checked. Racing components appear in the existing typed component panel. Gate orientation defines local +Y as the permitted crossing direction and local X/Z as the gate rectangle.

The generated controls use the existing input actions: move_x/move_y drive, jump is handbrake, crouch is recovery, interact toggles camera, and pause/restart/debug retain their named actions. These bindings are emitted in `game.dvegame`, including gamepad sticks and buttons. Focus loss/ejection uses the kit's existing input clearing.

## Handling and collision

The portable arcade controller is explicitly **not the optional Jolt wheeled-vehicle solver**. It uses longitudinal acceleration, service braking/reverse, speed-dependent bicycle steering, velocity damping along the car's lateral axis and reduced lateral grip during handbraking. Both steering and grip affect the trajectory.

A 120 Hz accumulator makes handling independent of the host's ordinary 60/120 Hz steps. Calls with invalid, nonpositive or greater-than-250-ms deltas are refused; there are at most 31 microsteps per call. Each microstep drives the car, resolves up to three continuous capsule sweeps/slides, follows ground with a downward query, then ticks the shared GameWorld. The car is a movable visual object with no separate rigid body, avoiding double simulation and self-query collisions. The body capsule is fixed to this starter car (0.96 m radius, endpoints ±0.7 m along forward, centre 1.1 m above origin).

This is intended for the supplied flat course. It has no wheel contact/suspension model, bank/roll alignment, reliable ramp/stair traversal, dynamic-body impulse exchange, animated wheels, drivetrain/gear simulation, terrain traction, AI racers, vehicle destruction or multiplayer authority. Car mesh resizing does not resize its collision capsule. The chase camera ray shortens around occluders; it is not a spring-arm volume sweep or smoothing simulation. Mouse look is unused while driving.

## Save and export

The versioned bounded `DVE_RACE 1` payload is attached to the runtime Rules component and travels through existing GameWorld/save-game component persistence. It carries race clocks, lap/gate progression, best lap, velocity, yaw, countdown, checkpoint, camera mode, debug flag and remaining step accumulator. Car transform is ordinary saved world state. Restore rebinds car/rules by stable key and gates by order, and validates the payload and scene before accepting it. Diagnostic trace text and input button latches are transient. Authored scenes do not receive runtime state on Stop.

The normal prototype exporter handles the racing project: editable scene, cooked polygon/marker assets, runtime manifest and input bindings. No new package or scene format is required. The generated README documents controls and `dve_pack` usage. The source manifest includes all changes.

## Verification

The new integration suite exercises directional and out-of-bounds gate rejection, duplicate orders, actual throttle/reverse/steering/braking, grip tuning, matching 60/120 Hz motion, thin-wall collision, ordered lap completion, pause/finish freezing, recovery penalties, restart/best behavior, state restore and malformed-state refusal. It also creates/undoes/redoes the native starter, exports and boots the project, checks countdown/input, runs editor Play/Stop, saves/loads through PlayerApp, renders a CPU framebuffer and boots a `.dvepak`.

Windowed SDL driving and gamepad feel still require user acceptance; a headless screenshot/test run does not establish handling polish. The existing first-person starter remains covered by its own integration suite.

## Local acceptance record

Six focused suites passed: racing integration, existing first-person prototype integration, editor Play session, player runtime, runtime scene loader and editor scene export. The generator CLI created a complete fresh project. The CPU chase-camera/HUD framebuffer was inspected. All 62 shader contracts and validator self-tests passed. The source manifest and whitespace checks passed. Reference physics and Lua OFF were used; SDL3 was unavailable, so a windowed driving/gamepad session was not performed.
