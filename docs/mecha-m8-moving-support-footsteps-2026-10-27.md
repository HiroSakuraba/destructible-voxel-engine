# M8 — moving-platform foot IK, bounded ankle articulation, foot-contact events

Built on merged M6 (#248) and M7 (#249).

## Runtime changes

M7 held feet in world coordinates and could not distinguish contact with a moving floor from contact with a static one. M8 remembers **which collider supports each planted foot** and stores the ankle position in that object's local transform frame. On each stance update, the current `GameWorld::moving_transform` reconstructs the corresponding world-space contact position, including translation and rotation. The normal 3D raycast must still confirm support, and a suddenly destroyed/disabled/unreachable support releases the plant.

This is *visual foot attachment*. It does not move the character motor along with the platform; physical character-on-platform transport and friction must be handled at the motor/physics layer separately.

M8 also limits rendered ankle slope articulation to approximately 35° while preserving the actual ground normal for support logic. It avoids impossible-looking foot rotations on very steep faces without falsely flattening the contact geometry.

## Footstep events (future particle/audio integration)

`MechaGame::footstep_events()` returns a transient span valid until the next update. Every event carries:
- World-space landing position and ground normal.
- Support-object identifier, left/right side, and normalized landing intensity.
- A maximum of two contacts in one fixed step.

Events only occur on the swing-to-ground transition, not when the game starts, idles, reloads, or repeatedly probes a stationary foot. This enables future particle, sound, decal, rumble and camera-shake systems to share one source of truth. `PrototypeGame::mecha_footstep_events()` exposes the event stream to the standard game host, and each confirmed footfall creates a small dust preview through the **existing CPU impact-visual path**. This is intentionally **not** a new particle renderer or GPU emitter binding.

`MechFootContact` exposes `supportObject` for diagnostics and tests.

## Compatibility and testing

The serializable `DVE_MECHA 3` payload stays unchanged. Cached local anchors are derived from authoritative restored world transforms and the previously saved foot positions. No engine save-schema bump is required.

`dve_mecha_m8_tests` tests:
- Dynamic polygon support translating under both planted feet and deletion releasing both anchors.
- Real bounded one-shot footstep events while walking, with no events when standing still.
- Slope-tilt clamping and invalid normal fallback.
- Version-3 save roundtrip and reconstructed support identities without fake load-time footsteps.

```sh
cmake --build build --target dve_mecha_m8_tests dve_mecha_m7_tests dve_mecha_m6_tests
ctest --test-dir build -R "dve_mecha_(m8|m7|m6|m5|m3_m4|game)_tests" --output-on-failure
```

## DVE particle system — source audit and proposed follow-up

Current implementation:
- A graph asset/compiler and deterministic `VfxCpuRuntime` in `vfx_particle_graph.*`. Spawn/update/render modules include rates/bursts, shape/velocity initialization, gravity/drag, approximate curl-noise, collision, color/size/emission over lifetime and renderer descriptions.
- `SpriteParticleSystem` renders particles, flipbooks, ribbons/trails and beams into `SpriteRenderList` with texture and ordering-based batching.
- `plan_vfx_gpu_frame` provides an **execution schedule** (reset/spawn/update/events/compaction/sort/indirect), but the repository search shows no renderer/backend invoking that API. This is a plan, **not yet a measured GPU particle system**.

Recommended next tranche **P1: practical 3D combat VFX**:
1. Event-to-emitter bridge: convert footstep, rocket impact, hit/debris events into named and cached effect instances. Limit per-event emission and prevent playback/re-emission after quickload.
2. World-aware collision: provide Z-up signed-distance / depth / voxel collider sampling behind a query abstraction; keep CPU authoritative where actual interaction matters. Avoid sending one `GameWorld::raycast` per high-count cosmetic particle.
3. Budget management: emitter/particle caps, per-effect quotas, distance/visibility LOD, fixed-cost overflow policy, profiler timings and dropped-event accounting.
4. Physically legible presets: blast core and secondary smoke, muzzle flash/tracers, dust puffs, shrapnel sparks, voxel debris, coolant, sparks and jet contrails.
5. Only then implement and benchmark a backend GPU kernel + indirect renderer with CPU/GPU comparison harness and device loss/reload tests.

Separate **P2**: artist preview/editor tooling, Niagara/VFX-Graph-style subgraphs and curves, flipbook selection, module presets and hot reload; **P3**: particle lights, soft intersections, transparency sorting / weighted OIT, material-aware collision/decal staging, GPU readback minimization.


## Particle correctness audit — concrete findings

The following source-level concerns are worth fixing before increasing particle count:

1. **Coordinate conventions:** `VfxCpuRuntime`'s `GroundCollision` checks particle **Y** against a horizontal plane. The mecha/voxel world is **Z-up**. A 3D world-space collision API must explicitly configure up axis or use geometry queries; reusing the current 2D plane blindly will make dust and debris bounce in the wrong direction.
2. **Generation identity:** the slot-reuse path in `VfxCpuRuntime::step` assigns `particle = {}` before `++particle.generation`. This resets the generation to zero each time, so a recycled slot receives generation 1 instead of a monotone version. Event consumers cannot reliably identify a reused particle. Preserve the prior generation before reinitializing.
3. **Upfront CPU capacity:** each `VfxCpuRuntime` allocates `maximumParticles` slots in its constructor, even if the graph spawns only a few particles. `SpriteParticleSystem` defaults to 256 emitter slots. These are **limits**, not an observed performance result; test actual memory and cache residency before scaling emitter counts.
4. **CPU draw cost:** `SpriteParticleSystem::build_render_packet` creates a full draw item per living particle and stable-sorts the list; this is appropriate for small-to-medium 2D effects but not a substitute for a GPU indirect 3D renderer.
5. **No GPU execution verification:** `plan_vfx_gpu_frame` calculates dispatch dimensions and buffer estimates. Repository code search found no actual caller that submits these passes. Measure only once a real shader executor and a visible 3D render path are wired up.

For an implementation reference, Unreal Niagara offers CPU ray collisions and GPU depth/distance-field collisions, showing the useful accuracy/performance split: https://dev.epicgames.com/documentation/en-us/unreal-engine/particle-update-group-reference-for-niagara-effects-in-unreal-engine. Unity VFX Graph offers event payloads and GPU subgraphs: https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.visualeffectgraph/Documentation~/Events.md. AMD's Parallel Sort can later be benchmarked against unsorted/weighted blended transparency for many overlapping sprites: https://gpuopen.com/fidelityfx-parallel-sort/. These are architectural references, not evidence that corresponding DVE backends are implemented.
