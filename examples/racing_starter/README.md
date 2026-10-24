# Racing time trial

Build `dve_create_racing`, then run:

```sh
dve_create_racing NEW_PROJECT_DIRECTORY
dve_player NEW_PROJECT_DIRECTORY
```

Alternatively use **Tools > Gameplay Prototype Kit > Create racing time trial** in an empty scene. Every starter object is added in one undo step. Export through that panel to create a fresh editable/standalone project.

The starter is a three-lap single-car time trial on a flat four-gate circuit. Follow the arrows counterclockwise, passing gates 1, 2, 3, then finish gate 0. Wrong-way crossings and finishing before the ordered checkpoints do not advance the lap. The three-second countdown locks the car.

| Keyboard | Gamepad | Action |
|---|---|---|
| W/S | Left stick Y | Throttle, service brake and reverse |
| A/D | Left stick X | Steering |
| B | Left trigger | Independent service brake |
| Space | South | Handbrake |
| Ctrl | East | Recover at last passed gate; five-second penalty |
| E | West | Chase/hood camera |
| P | Start | Pause/resume |
| R | Back | New attempt, retaining session best lap |
| F3 | — | Runtime diagnostic toggle |

Asphalt and grass now have independent `race_surface` grip/rolling-drag properties. Grass is a collision-enabled thin layer so the ground probe distinguishes it from the asphalt underneath. Skid intensity is available to downstream effects, but particles, tire marks and tire audio are not yet connected.

Select a street, arcade, drift, offroad or track profile in the Race Car component. The F3 HUD shows lateral slip, yaw rate and contact counters. Presets apply gameplay multipliers to the existing grip, steering, acceleration and braking settings. Collision remains portable (without Jolt).

The generated folder contains `project.dveproject`, `scenes/prototype.dvescene`, cooked scene/mesh assets and `game.dvegame` with bindings. Existing output folders are refused. Package with `dve_pack NEW_PROJECT_DIRECTORY racing.dvepak --all` and launch the package with `dve_player racing.dvepak`.

This is a portable arcade controller with a swept body capsule and simple Z-up ground following. Wheel geometry is cosmetic; Jolt suspension, tire animation, realistic powertrain, slopes/ramps, AI opponents, damage and network racing are not part of this starter. Best lap survives restart within a session and save/load; it is not a separate permanent leaderboard.
