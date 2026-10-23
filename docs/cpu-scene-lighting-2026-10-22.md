# CPU scene lights and multi-bounce transport

This implements the requested lighting families in the CPU reference renderers and CPU player. It does **not** establish complete GPU or offline-renderer parity. REN-020 and REN-021 now have a working CPU implementation; their full specification acceptance criteria remain broader than this change.

## Features

| Lighting | Behavior |
|---|---|
| Point | Inverse-square RGB radiant intensity, metric positions, opaque visibility rays. |
| Spot | Point emission with a smooth inner/outer cone, power weighted by its integrated cone distribution. |
| Area | Quad and elliptical disc emitters, uniformly sampled area, solid-angle conversion, one/two-sided emission and soft shadows. `positionMeters` is the emitter **centre**; quad edges are full edges spanning ±½ edge around it; disc edges are radius vectors. The cross product of the edges determines the emitting normal. |
| Emissive geometry | Constant material emission on mesh triangles and exposed voxel faces enters the sampled light list. Internal voxel faces are excluded. Emissive materials include their base color as the existing beauty renderer does. |
| Candle | A point emitter with bounded, seeded, continuous flicker. Position, color, intensity, frequency and amplitude remain editable. The player uses saved simulation time, so pausing stops flicker. A warm color is authored explicitly. This does not generate flame geometry. |
| Glow | Separable HDR bloom before exposure and tone mapping in the CPU player. Threshold, intensity and a bounded pixel radius are editable. Bloom is optical appearance; it does not inject indirect energy into the scene. |
| GI / multiple bounces | Iterative diffuse/rough-GGX/clear-coat transport, throughput weighting, next-event estimation, escaped environment and shadowed sun. Optional survival-reweighted Russian roulette starts after three secondary vertices. |
| Fog | The same scene emitters feed single scattering in local CPU fog. Both geometry visibility and optional volume shadow transmission are included. |

Voxel and polygon surfaces use the same frame integrator in hybrid rendering and CPU Play, including cross-geometry local-light shadows and bounce hits. The new path solver splits indirect diffuse/specular by the first scattering lobe, retaining the existing AOV decomposition. Sun shadows on polygons are evaluated when this integrator is active. When the integrator is active (any explicit light or a positive path budget), the hybrid/player voxel sun pass also receives polygon sun occluders and the CPU player applies bloom; legacy scenes keep their previous player frames bit-for-bit.

## Authoring and persistence

`RenderEnvironment::lights` holds up to 4096 explicit lights. Positions and area vectors use **metres**, independent of the renderer's voxel/world unit scale. Point/spot/candle `intensity * color` is radiant intensity; area values are radiance. Consequently a white Lambertian surface with zero specular facing a point light at distance `r` returns `intensity / (pi * r*r)`. Existing sun intensity retains the engine's older white-surface irradiance convention, including its pi scale. This distinction avoids silently changing existing sun-lit images.

The registered `dve.light` component exposes kind, local offset, direction, area edges, linear color, intensity, cone angles, sidedness and candle parameters through the existing component system. Its offset and edges are in metres and follow the owner's rigid world frame; geometry voxel-scale changes do not resize a light. Enabled components are collected from the running world by `PlayerApp::environment()`. Existing component scene/prefab/save serialization carries these records. Light components can be attached to markers or geometry. This change does not add a dedicated light-placement gizmo or a new component-picker UI.

Lua can replace or append a light with a one-based index; passing `nil` removes it. Replacement starts from defaults, rather than patching the previous record:

```lua
assert(world.set_scene_light(1, {
    kind = 'quad',
    position = {x=0, y=3, z=0},
    edge_u = {x=1, y=0, z=0},
    edge_v = {x=0, y=0, z=1}, -- cross(U,V) points down
    color = {x=1, y=.92, z=.8}, intensity = 12
}))
assert(world.set_scene_light(2, {
    kind = 'candle', position = {x=1, y=1, z=0},
    color = {x=1, y=.42, z=.10}, intensity = .3,
    flicker_amount = .15, flicker_frequency = 8, seed = 47
}))
assert(world.set_environment_scalar('PathTraceBounces', 6))
assert(world.set_environment_scalar('PathTraceSamples', 16))
assert(world.set_environment_scalar('LocalLightSamples', 8))
```

Script state version 5 adds a length-prefixed `DVE_LIGHTING 1` extension. Version 1–4 states still load with an empty explicit light list and legacy bounce policy. Parsing is bounded, validates all records and commits atomically. Rejected script setters retain the previous state. C++ `encode_scene_lighting` / `decode_scene_lighting` provide the same portable extension independently of Lua. Authored component lights are collected separately, so loading a script-state light list does not duplicate components.

Editor project settings expose `render.path_bounces`, `render.path_samples`, `render.local_light_samples`, `render.path_roulette`, `render.bloom_intensity` and `render.bloom_radius`. These resolve into the Play environment through the existing settings adapter. The software marker viewport's Lighting Only view is still its existing sun/ambient approximation; it is not the CPU player image.

## Integrator contract and budgets

- `pathTraceBounces = 0` preserves legacy GI. Positive values allow 1–64 secondary vertices, with 1–256 independent paths per primary hit. Emissive-only scenes should select a positive path budget to activate the new integrator; an explicit light list also activates its direct-lighting path.
- 1–64 power-weighted next-event samples are taken per vertex. Emitter selection uses a cumulative distribution; area PDFs convert to solid angle and include the selection probability.
- Primary direct lighting uses the light-sampling estimator alone. Corresponding first-continuation emitter hits are excluded to avoid counting them twice and to keep the artistic GI intensity independent of direct energy. Later vertices combine NEE and continuation emission with the power heuristic. At a terminal vertex NEE has no competing continuation and receives full weight.
- Continuation directions use a cosine hemisphere distribution for all supported lobes. This covers diffuse and finite-width GGX/coat lobes, but narrow glossy reflections can have substantial variance. It is not a delta-specular sampler.
- Russian roulette changes variance/cost and reweights survivors. The depth cap, per-segment GI distance and ray-origin bias remain finite preview approximations; this is not an infinite-depth unbiased claim. The GI intensity scales the indirect contribution once, rather than at every bounce.
- `Fast` player quality disables path continuation and uses up to two emitter samples. `Balanced` caps paths and bounce count at two, and emitter samples at two. `Reference` preserves authored budgets. Budgets are requests per shading sample, not a guaranteed frame-time limit.
- The integrator is rebuilt per render and reused by voxel, polygon and fog passes. Instance arrays, material storage and geometry must remain alive and immutable during queries. Queries have no mutable random state and produce identical voxel images across worker counts.
- Bloom is applied once by the CPU player, after fog, when the integrator is active. Standalone/hybrid linear reference callers explicitly invoke `apply_cpu_bloom` when desired. Its glow is added to the emission AOV, preserving the light-pass sum; shadow-removal comparisons should use the pre-bloom linear image.

## Limits and remaining acceptance

The new secondary tracer and local-light visibility treat surfaces as opaque. Texture/vertex-varying secondary materials, alpha-cutout visibility, transmissive shadows, refraction and emitter-fed caustic photons remain work. Mesh emitter sampling uses constant material emission, with the instance tint applied to the base-colour part only (matching the beauty pass); it does not importance sample emissive textures. The existing sun-fed caustic/subsurface paths remain separate. Surface and volumetric multiple scattering are different problems: this change adds multiple surface bounces and single fog scattering.

Explicit area lights appear in secondary rays and shadows. To draw a visible area-light shape in the primary raster image, author an emissive mesh; do not duplicate its energy with an overlapping explicit area source. Additive light components and script lights are also separate authored sources.

No new GPU emitter buffers, dispatches or GI acceleration are included. The GPU environment pack continues to carry the older sun/GI contract. A dedicated editor light-placement workflow, GPU parity, delta reflection/refraction sampling, denoising/progressive accumulation, REN-023 emitter-fed photons, and REN-027 checked-in Cornell image acceptance remain outstanding. The analytic tests below are not a substitute for the Cornell reference.

## Validation

The selected headless suites cover scene lighting, environment validation, Lua bindings, game saves (including older script versions), player boot/rendering, hybrid composition, radiance cascades, ambient occlusion, specular shading, fog quality and AOV reconstruction.

New numerical tests cover:

- Analytic point irradiance and metric world-scale invariance; spot axis and cone exclusion.
- A unit disc at height two: white diffuse response `R^2/(R^2+h^2) = 0.2`, within 1%.
- Doubling independent emitter samples approximately halves measured variance.
- Opaque polygon/voxel visibility; exposed emitting voxel faces and exclusion of internal faces.
- A closed diffuse enclosure with reflectance 0.8 and unit wall emission. Reflected radiance is `0.8/(1-0.8)=4`, or total radiance 5 after including the primary wall's own unit emission. Fixed-depth and roulette estimates must agree within 1% in the deterministic test.
- Diffuse/metallic indirect-lobe separation, component validation/placement, deterministic candle bounds, emitter-fed fog, bloom spread, atomic malformed-input rejection, codec round trips, native voxel/polygon AOV sums and worker-count determinism.
- Player collection of a moving light component, saved simulation time, and all three quality policies.

Algorithm reference: [PBRT, A Better Path Tracer](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer). Test contracts above refer to this implementation's specified units and finite budgets.

Final local verification: **11/11 selected CTest suites passed** (about 35 seconds). The new lighting suite also passed focused AddressSanitizer + UndefinedBehaviorSanitizer checks with `scene_lighting.cpp`, `render_environment.cpp`, `component.cpp` and the test instrumented. Dependency archives were uninstrumented; leak detection was disabled. Editor settings source and runtime-settings header passed standalone syntax checks. No windowed artist acceptance or GPU acceptance is claimed.
