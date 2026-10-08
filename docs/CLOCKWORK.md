# Clockwork integration

Clockwork gives the player, play-in-editor, sprite-level preview, Fluoddity and 2D backends
one scheduling implementation. Each host owns an instance; it is not a global singleton.
The world remains a serial mutation owner. Solver substeps remain backend settings.

## Host scheduling and input

`SimulationClock` accepts integer nanoseconds and carries rate-scaled integer phase.
Integral simulation frequencies such as 60 and 120 Hz do not round their individual periods.
The `double` adapter carries fractional nanoseconds between calls. Interactive defaults accept
at most 250 ms and execute at most eight steps. `BacklogPolicy::Drop` discards excess complete
ticks while retaining fractional phase; `Retain` preserves debt. Dropped time is observable.
Interpolation alpha is clamped, including when debt remains.

```cpp
#include "dve/simulation_clock.hpp"
#include "dve/game_world.hpp"

void frame(dve::GameWorld& world, dve::SimulationClock& clock,
           std::uint64_t elapsedNanoseconds) {
    world.set_frame_presentation(true);
    const auto steps = clock.advance_nanoseconds(elapsedNanoseconds);
    for (std::uint32_t i = 0; i < steps; ++i)
        world.tick(clock.fixedDeltaSeconds);
    world.update_presentation(float(std::min(double(elapsedNanoseconds) / 1e9, 0.25)),
                              clock.alpha());
    const auto objects = world.render_objects(clock.alpha());
    // Submit objects to the renderer; do not write their interpolated poses to physics.
}
```

A host with its own fixed clock calls `Physics2DWorld::step_fixed(dt)`, avoiding a second
accumulator. Standalone 2D clients can continue using `step(frameDelta)`. Exact display
release times come from `tick_deadline_nanoseconds(releaseIndex, TickRate{165, 1})` relative
to one epoch. `RationalTimeScale` carries remainders for rational speed conversions; existing
editor settings continue accepting floating time scales through the clock's double adapter.

The SDL player normalizes timestamped input into `(tick, sequence)` at the host boundary.
Events in discarded wall time go to the next unsimulated tick. Equal-tick events retain their
insertion order. `PlayerInput::tick_events()` exposes every edge, and `press_count(action)`
counts multiple presses in a tick; the existing boolean action API retains its compatibility
behavior. Record these normalized events for playback, rather than deriving ticks from wall
clock timestamps again. Loading a player save clears input queued for the previous timeline.

## World time and phases

World time is an integer tick count plus a fixed step. `elapsed_seconds()` is derived,
not accumulated. Use one fixed rate for a session. The compatibility path for an embedder
changing its step rebases world tick units and timer deadlines; the world counter therefore
is not a lifetime step counter across rate changes. The player's step counter remains separate.

The compiled phase table checks prerequisites and uniqueness at compile time:
clock, timers, queued destruction, physics, animation, root motion, gameplay, secondary
motion, listeners. These are named functions in the existing order, with destruction inserted
before physics. Timers and arbitrary callbacks stay exclusive. New tick listeners registered
inside a listener become eligible on the following tick.

Repeating timers derive each release from an origin and a release index. For example,
25 ms releases at 60 Hz alternate tick gaps instead of rounding every interval to two ticks.
Due callbacks are snapshotted before dispatch; a callback cannot recursively create a timer
that fires in the same dispatch. A repeating timer fires at most once per world tick.

`SystemCadence{every, offset}` is an integer cadence helper for explicitly scheduled systems.
It does not automatically parallelize or decimate physics, Lua, hair or other existing systems.
The job pool serializes concurrent coordinators, executes nested work on the same pool locally,
and propagates worker exceptions after the batch joins. Cross-pool dependency cycles remain
an application error.

## Presentation and character movement

Native player and play-in-editor hosts opt into frame presentation. Camera target smoothing
and UI rebuilds run per frame. Cinematic sequence time, shot commands and events still run on
world ticks, before Lua listeners. Rendering a frame does not advance cinematic events.
Sequence pose evaluation and object transforms can interpolate without changing simulation.
Older embedders retain tick-driven presentation until they opt in.

Transform history includes scripted markers and other moving objects. External transform
writes, object removal and save restore reset the relevant history. A script that deliberately
teleports inside a tick should call `reset_presentation_history()` after the teleport. The
player renderer consumes interpolated objects. The editor voxel draw list consumes a separate
presentation pose view and invalidates its culling cache when those poses change. The authored
document retains authoritative tick poses.

Registered gameplay characters queue root displacement and resolve it through the same
capsule sweep, slide and step-up path as player movement:

```cpp
if (!world.gameplay().queue_root_motion(pawn, worldSpaceDisplacement)) {
    // Register the pawn as a gameplay character, or use the appropriate backend controller.
}
```

Animation-driven root motion uses this path automatically for registered characters.
Non-character animation objects retain their existing direct movement. Character motion still
runs after the rigid-body solve: its sweep resolves collision immediately, but dynamic-body
solver response occurs on the next solve. This does not introduce a Jolt kinematic-target API.
Queued root intents survive saves and are included in replay fingerprints.

Reference-backend forces and torques now accumulate until the next step, where impulses use
that step's actual duration. `apply_impulse` remains immediate. Reapply a sustained force each
tick; it is cleared after integration.

## Audio and music clocks

The SDL audio callback publishes a bounded atomic clock anchor containing host time, generated
sample frame, sample rate, device generation and estimated queued frames. Producers subtract
that latency when mapping an intended audible host time. Old timestamps clamp to the earliest
writable frame, without unsigned underflow. An explicit generation mismatch rejects a command;
future starts from a previous device generation are cancelled when the renderer drains them.
Already playing sources continue through a device restart.

```cpp
#include "dve/audio/mixer.hpp"
#include "dve/audio/audio_clock.hpp"

dve::audio::PlaySampleDesc event;
event.sample = clickSample;
event.spatialized = false;
event.audibleHostNanoseconds = dve::audio::audio_host_nanoseconds() + 20'000'000;
if (const auto anchor = mixer.audio_clock().anchor()) {
    event.deviceGeneration = anchor->deviceGeneration;
    const auto handle = mixer.play_sample(event);
    // An invalid handle reports queue rejection, missing asset or a stale clock.
}
```

This is an estimate of output-buffer latency, not a hardware presentation timestamp. SDL's
host clock for input and the steady-clock epoch for audio are different domains: use
`audio_host_nanoseconds()` for these audio requests. RtMidi preserves callback arrival time,
source delta and source sequence; it does not invent a hardware timestamp from the delta.
The synthesizer also assigns insertion sequence so equal-frame deferred MIDI keeps its order.

`Synthesizer::set_game_music_clock` publishes one latest beat/tempo/transport anchor, applied
at its specified sample frame. It supports pause, resume and generation changes. Arpeggiator
release frames derive from absolute beat positions, including swing, so rounded step durations
do not accumulate drift. Human timing remains intentional deviation from that grid.

```cpp
// One game/transport-thread publisher. Choose a future frame when scheduling ahead.
dve::audio::GameMusicClockAnchor music;
music.sampleFrame = synth.current_frame() + 512;
music.beat = 16.0;
music.tempoBpm = 123.0F;
music.running = true;
music.transportGeneration = 2;
synth.set_game_music_clock(music);
```

The mailbox coalesces superseded anchors; it is not a queue of every future tempo change.
Arpeggiator presets must select `GameClock`. Existing tempo-synced effects continue using
that tempo; this change does not phase-reset every LFO or generative sequencer. Game authors
publish beat anchors explicitly; world ticks do not automatically determine a musical tempo.

## Resumable destruction

`damage_sphere` keeps its synchronous behavior. `queue_damage_sphere` opts into preparation
that can suspend between ticks. Raster, connectivity, geometry/mass and proxy work use a
logical unit budget, rather than elapsed microseconds. FIFO results commit before physics;
partial geometry is never exposed. Source geometry or mass changes invalidate staged work.
Native replacement bodies are created before publishing fragments, and backend creation
failure rejects the transaction without replacing live geometry.

```cpp
world.set_destruction_units_per_tick(4096);
const auto request = world.queue_damage_sphere(object, hitPoint, 0.6F);
// Observe the ordinary damage callback when the request commits.
// pending_destruction_count() and rejected_destruction_requests() expose progress/admission.
```

Admission limits are 64 queued requests, 32 connected pieces, one million voxels and an
extent of 4096 per piece, 256 unmerged proxy boxes per piece, and the configured debris cap.
Exceeding a limit rejects the entire request. The proxy limit can reject an object that the
synchronous path accepts after box merging. Map allocation, a brick operation and native
body creation still have variable wall time: this is deterministic work budgeting, not a
hard real-time bound. Profile memory and commit time before making the queued path the default.
Saving pending work records its logical cursor; loading reconstructs that preparation before
resuming. A source already invalidated at save time remains invalidated after restore.

## Saves and replay validation

Save schema 3 adds integer clock/timer state, destruction cursors and session bookkeeping.
The `dve.root-motion` section stores unconsumed character intents. Schema 0/1/2 migrations
remain supported; float time is converted once on restore. Legacy world hashes included
accumulated float time and cannot be compared with the new hash definition. The player still
verifies container and source-asset integrity, and compares the new world hash for integer-clock
saves. Ordinary save restore remains distinct from backend solver rollback.

```cpp
#include "dve/runtime_replay.hpp"

std::string error;
const auto expected = dve::capture_runtime_replay_checkpoint(world, codec,
    scriptState, {{"game-rng", rngBytes}}, &error);
// Run the same normalized tick input through another identically configured world.
const auto actual = dve::capture_runtime_replay_checkpoint(otherWorld, codec,
    otherScriptState, {{"game-rng", otherRngBytes}}, &error);
if (expected && actual) {
    const auto divergentSections = dve::compare_runtime_replay_checkpoints(*expected, *actual);
    // Report subsystem names, rather than accepting a matching visible-world hash alone.
}
```

`PlayerApp::replay_checkpoint()` adds bindings, held/edge input and pending normalized events,
and invokes the script save hook when Lua is enabled. Fingerprints include serialized gameplay,
animation, rigs, ragdolls and hair; world session state and pending destruction; authoritative
camera sequence state/assets; reference solver pending loads, quiet counters, constraints and
allocation state; or the native Jolt snapshot, masks, body/character states and contact time.
Frame camera smoothing and renderer state are excluded.

These are comparison fingerprints, not a generic rollback format or a proof of cross-platform
bitwise determinism. Run identical content/configuration/backend builds. The application must
supply external RNG, custom state, script state outside its save hook and any other future-
affecting state. Active deformables require an explicit application section; unsupported
physics backends fail capture instead of claiming complete solver coverage.

## Verification

`dve_clockwork_tests` checks exact simulation counts across 60/120/144/165/240 Hz host releases,
backlog policies, rational remainder, repeating timer cadence, callback ordering, transform
history, tick-only cinematic events, force integration at 30/60/120 Hz, multiple input edges,
root sweep collision and intent serialization, destruction save/continuation and stale-source
rejection, replay divergence, audible sample starts, beat-phase pause/resume and job-pool safety.
Existing world, save, gameplay, camera, player, 2D, audio and editor tests cover compatibility.
SDL contracts test worker wake coalescing and absolute deadline rounding. Native SDL/Jolt builds
and upstream/extended Jolt tests verify the production adapters; physical audio/MIDI devices
and gameplay feel still need hardware/playtesting.
