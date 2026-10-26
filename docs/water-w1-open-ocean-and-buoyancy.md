# W1 — Deterministic water and two-vessel buoyancy test range

The first water/ships PR adds a **playable water test range** with a dinghy and pirate ship. It starts the common water architecture, without conflating appearance, ocean mechanics, and the vessel's six-degree-of-freedom motion.

## Core data and physics contracts

`OceanField` supplies deterministic, GPU-free `sample(x,y,simulationTime)` queries. Each sample returns a world-space wave **height**, geometric **normal**, orbital **water velocity**, and bounded **foam propensity**. All gameplay and visual water surfaces call the same evaluator. Ocean settings expose Calm, Breeze and Storm presets, with bounded artist-customizable waves and horizontal current. Wave parameters use metres, seconds, normalized directions and an invertible Gerstner steepness budget. The height query numerically inverts horizontal Gerstner displacement; it does not confuse undisplaced parameter space with world-space sample points.

`evaluate_water_hull()` distributes Archimedean buoyancy over a configurable probe grid, applying force and torque at each immersed hull location. It also applies damping relative to local wave orbital velocity and horizontal current. The portable `step_water_hull()` controller integrates six-degree-of-freedom boat motion at 120 Hz with a diagonal inertia approximation, separately from static land collision. Neither waveform nor buoyancy requires Jolt or a GPU.

### Reference craft

| Parameter | Dinghy | Pirate ship |
|---|---:|---:|
| Mass | 480 kg | 24,000 kg |
| Max displaced water | 0.56 m³ | 27 m³ |
| Hull length | 4.3 m | 26 m |
| Beam | 1.8 m | 7 m |
| Draft | 0.8 m | 3.2 m |
| Probe layout | 3 × 3 | 9 × 5 |

Both vessels independently react to the same waves. The active vessel alone receives the demo motor and rudder inputs, enabling comparative pitch/roll and inertia tests. Ocean and hull values are editable component properties; authoring rejects nonfinite, excessive or inconsistent values.

## Rendering

The CPU player draws water as a separate depth-aware ocean overlay after the scene resolve and before gameplay HUD. Its height/normal/velocity come from the authoritative `OceanField`, never from a duplicate visual sine animation. The half-resolution 2×2 shading block reduces CPU work and checks depth against regular polygon and voxel scene geometry, allowing piers/boats to occlude water. First-pass color includes crest foam, directional lighting and a Fresnel-inspired viewing-angle response. It is **not yet** a full physically based water shader: planar reflections, refraction, transparent underwater views, caustics and soft blending remain future rendering work. The editor's static viewport does not yet display the ocean overlay; it appears in the player game render.

## Gameplay and editor

The `make_water_starter()` harbor contains a dinghy, pirate ship with a simple sail silhouette, static collidable harbor geometry and a bodyless ocean marker. It can be created from the Gameplay Prototype Kit and exported using the existing project exporter or by running `dve_create_water NEW_PROJECT_DIRECTORY`.

Controls: `W/S` forward/reverse thrust; `A/D` rudder; `1` dinghy; `2` pirate ship; `E` toggle boats; `P` pause; `R` reset; `F3` hull diagnostics. Gamepad left stick, West/Start/Back are bound. W1's motor and rudder are explicitly simplified prototype controls. Actual sails, apparent wind, keel force, propeller dynamics, planing lift, foils and hydrodynamic lift are W2.

Ocean physics is a 120 Hz deterministic simulation; the CPU rendering pass accepts the same saved simulation time. The `DVE_WATER 1` versioned state tracks both hulls' positions, quaternions, velocities, selected craft, pause and accumulator, and remains compatible with the existing GameWorld save/load pathway.

## Validation

- `dve_water_ocean_tests`: all sea-state presets, finite normals/orbital velocities, custom wave validation, transactional rejection, buoyancy above/below water, bounded settling and changing wave height.
- `dve_water_game_tests`: editor scene validation, generated project loading, genuine player input, both vessels moving, save/load round trip and depth-aware ocean framebuffer rendering.
- Existing racing, flight and first-person projects should remain unchanged.

### Explicit W1 limitations

This is a buoyancy **probe** approximation, not a full volume-clipping ship hydrostatics solver. Hulls use visual-only polygon geometry with separate swept land collision. Existing voxel/Jolt physics actors do not yet automatically float; a reusable force-at-point component is a sensible W2 follow-up. The open ocean is Gerstner-based, not an FFT spectrum. Wakes, hull slamming/planing, detailed sailor controls, foam simulation, tide/shoreline interactions, breaking surf, floating debris coupling, multiplayer wave synchronization tests and water-specific sound are not included.

Wave samples are designed for a shared future FFT/local-flow implementation. For W3 and W4, the interface should be extended to include local depth, breaking fraction, and spatial flow while retaining the existing hydrodynamics API.
