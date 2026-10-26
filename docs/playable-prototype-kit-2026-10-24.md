# Playable prototype kit — 24 October 2026

The kit connects existing character, input, trigger, scene export and packaging
systems into a small playable first-person game. `PrototypeGame` is shared by
editor Play and `PlayerApp`; the standalone game does not link the editor or
require Lua. Scenes without enabled prototype actors continue down their existing
runtime paths.

## Build a playable scene

1. Create an empty scene and open **Tools > Gameplay Prototype Kit**.
2. Choose **Create first-person starter**. This is one undoable command and refuses
   to replace a nonempty scene. It creates a floor/corridor, a Player Start marker,
   Game Rules marker, switch/gate, three pickups, checkpoint, damage strip, goal
   and a jump obstacle.
3. Close the panel and press Play. WASD moves, Space jumps, Ctrl crouches, E uses a
   switch, P pauses game simulation, R starts a new round and F3 shows the trace.
   Escape/F8 ejects the prototype camera in the editor. Pause and single-step in
   the existing Play controls remain available independently.
4. Stop returns the authored scene. Runtime pickup/door flags, health, trigger
   state and save metadata do not leak into the authored components.
5. With a project open, the kit's **Export playable project** command creates a
   fresh `prototype-build-TIMESTAMP` folder. The command-line generator
   `dve_create_prototype NEW_DIRECTORY` creates the same complete starter directly.
   Both routes refuse to overwrite existing project output.
6. Run `dve_player NEW_DIRECTORY`, or package it with
   `dve_pack NEW_DIRECTORY NEW_GAME.dvepak --all` and run the package.

The generated project includes `project.dveproject`, the editable transactional
scene, a runtime scene and cooked geometry, `game.dvegame` with keyboard/gamepad
bindings, and a short README. The generated first-person example is reproducible
from source rather than a separate binary mesh collection that can drift from the
template generator.

## Coordinates and player setup

`GameplayRuntime` uses an XY horizontal plane with **Z up**. The prototype follows
that convention, uses a marker for the capsule's foot position, and sets the
session's physics gravity to negative Z. It does not reinterpret the existing
controller as Y up. The supplied example's geometry is authored accordingly.

Exactly one enabled `dve.prototype.player_start` role and one
`dve.prototype.rules` role are required. Player Start must have no geometry/body.
The shared existing capsule controller handles sweep/slide, slopes, steps,
crouching, coyote time and jump buffering. The kit feeds camera-relative movement
and press-edge jump input into it. Player controls expose speed, jump speed,
standing height, capsule radius, maximum health and look speed. Invalid capsule
configurations and out-of-range numbers fail startup with an object diagnostic.

The possessed first-person camera follows the capsule, including crouch eye
height. Mouse and gamepad/keyboard look use bounded pitch. The editor releases
pointer capture when paused/ejected. The standalone player requests relative
mouse input only while focused and playing; unsupported hosts retain ordinary
motion. No third-person animation/controller template is claimed in this batch.

## Components

Each gameplay actor has one role component with a unique, case-sensitive `key`.
Keys are 1–64 ASCII letters, digits, dot, underscore or hyphen. Connections use
these keys, so editor IDs, exported runtime IDs and scene display names can differ
without changing wiring. Duplicated/prefab actors need distinct keys; validation
refuses ambiguity instead of silently targeting the wrong copy.

| Role | Editable behavior | Events |
| --- | --- | --- |
| Player Start | Movement/capsule/health/look settings | `died`, `respawned` |
| Game Rules | Required item count, automatic respawn, kill-plane Z | `won`, `lost`, `restarted` |
| Switch | Reach and interaction prompt | `used` |
| Door | Instant open/close entity gate | `opened`, `closed` |
| Pickup | Sphere radius and positive collection value | `collected` |
| Checkpoint | Sphere radius; marker position becomes respawn point | `entered` |
| Damage Volume | Oriented box half extents, damage and interval | `entered`, `damaged`, `exited` |
| Goal | Oriented box half extents | `entered`, `exited` |

Switch interaction chooses a nearby actor inside a forward cone and checks a
ray for blockers. Holding E does not repeat activations. Pickups are collected
once, disabled and counted; they do not create inventory items. Checkpoints retain
current kit progress on death. Damage uses per-volume cooldowns measured against
world simulation time. Automatic respawn restores health and clears the existing
controller's velocity/support/jump grace state.

Doors open instantly by disabling the gate entity and close by enabling it;
character queries and rendering honor that state. This is a blockout gate, not an
animated/kinematic door or a general moving-platform physics implementation.
Pickup/hazard/goal visuals in the template have collision disabled, while trigger
volumes provide gameplay detection. Authored custom versions should follow the
same separation.

## Event/action panel

Select an object and open the kit panel. Cycle a role and add it to the selected
object, or create a marker with that role. Cycle the selected prototype component
to edit its schema-backed fields; click a value, type and press Enter. Bool values
use `true`/`false`, vectors use the existing component inspector's vector format.
Use Previous/Next to reach additional fields.

**Add event connection** attaches a `dve.prototype.link`. Cycle the source event,
click its target field and pick an actual scene object from the list, then cycle
a compatible target action. Picking a target updates its stable key and adjusts
an incompatible action together in one undo transaction. Connections remain saved
ordinary components and survive scene save/export/package.

Supported actions:

- Door: `open`, `close`, `toggle`.
- Game Rules: `win`, `lose`, `restart`. `win` requires the configured item count.
- Player: `heal`, `damage`, `respawn`, with a numeric amount for health actions.
- Other roles: `enable`, `disable`.

The sample switch's `used` event toggles the gate; the goal's `entered` event
requests `game.win`. The inspector validates missing targets, duplicate keys,
invalid source events, incompatible target actions and numeric/type errors.
Intermediate incomplete links can be edited, but cannot start Play successfully.

Runtime dispatch is queued, deterministic and bounded. Up to 256 events are drained
per step; a cycle that exhausts the budget stops the game with a visible error.
The trace retains its most recent 64 lines. Trigger mailboxes and event queues
also have explicit caps, and trigger processing is ordered by trigger/object/event
IDs. This is a small event/action system; a visual scripting graph and per-object
Lua script lifecycle authoring remain separate work.

## Rules and HUD

Playing, paused, won and lost are explicit phases. Paused/won/lost states freeze
the world's physics, triggers, timers and script tick callbacks. Input continues
to be processed so pause and restart remain responsive. Game pause is separate
from the editor's session pause. Editor single-step advances a running prototype;
resume the game's P pause first if it is also paused.

Health, item count, interaction prompt, status and win/loss/pause text are shown
in the editor Play overlay and CPU player's framebuffer. The standalone HUD uses
a small built-in monospace font and health bar, so it works without SDL font or
Lua dependencies and is present in screenshots/readbacks. F3 exposes the bounded
event trace. This does not add a general visual HUD layout/binding editor.

A round restart resets kit pickup/door/enabled state, health, count, checkpoint,
look orientation and damage cooldowns. It does **not** roll back arbitrary Lua
changes, voxel destruction, spawned objects or external systems. Stop/reload the
scene to reset the entire world. Checkpoints are in-memory respawn locations;
ordinary save slots provide disk persistence.

## Export and save compatibility

The exporter now retains meaningful empty component objects as explicit
`dve.marker` actors, using a validated empty DVOX transport file and the existing
component extension. The scene loader creates an actual bodyless GameWorld marker
for that tag, rejecting any tagged asset with geometry. This keeps player/rules/
checkpoint markers in the packaged game without inventing tiny colliding meshes.
The base scene format and shader ABI are unchanged.

The kit stores a versioned `dve.prototype.state` payload on its runtime Rules
object. Existing world saves therefore carry phase, health, count, camera,
checkpoint, HUD/trace and damage cooldowns. Existing gameplay snapshots carry
character/player/trigger state and entity enabled flags carry door/pickup state.
`PlayerApp` validates/rebinds the kit after restoring the existing runtime sections
into its fresh session; a failed restore leaves the old session intact. This
metadata is created in runtime worlds, not written back to authored scenes.

Limits: 1,024 actors, 4,096 event links, 64 trace lines, 256 dispatched events per
step, bounded trigger mailbox and a bounded save payload. Roles are captured at
startup; changing role settings during Play is not a live authoring workflow.

## Validation

The dedicated integration suite covers component/key/link validation, the kit
panel's real target picker and undo, starter creation/history, scene persistence,
editor Play/Stop/single-step, exported marker transport, actual character input
and a closed gate collision, short-tap interaction, three pickups, checkpoint,
damage cooldown, save/load, pause/resume, death/respawn, win/restart, HUD framebuffer
stability across save/load, packaged gameplay, loss mode and bounded event cycles.
Existing character, scene loader/export, editor Play and player runtime suites are
included in focused verification. Final execution results are recorded in the PR;
no windowed artist acceptance is implied by headless tests.

Local execution: all seven focused suites passed on 24 October 2026: prototype
integration, GameWorld, gameplay runtime, scene loader, editor scene export, editor
Play session and player runtime. The build used Lua OFF and the reference physics
backend for the new integration cases. The actual CPU framebuffer/HUD was inspected;
all 62 shader contracts and validator self-tests passed. SDL3 is unavailable in
this workspace, so windowed capture/presentation was not executed.

Two integration corrections are part of this delivery: the starter explicitly
centres the editor's corner-origin cube primitive, and disabled entities are
excluded from GameWorld ray/sphere/capsule/depenetration queries. Editor input
clearing also releases previously published runtime actions/axes, preventing
sticky controls after focus loss or camera ejection.
