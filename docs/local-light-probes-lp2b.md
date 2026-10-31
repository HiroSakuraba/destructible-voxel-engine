# LP-2B: local probes reach the Vulkan PBR material shader

Status: **implemented in PR, opt-in scene-owned rendering path**; subject to
actual Vulkan shader compilation and hardware acceptance. This is not a new
automated scene-capture subsystem or probe editor workflow.

## What it changes

- `PlayerRenderView` can now reference a baked `IrradianceProbeGrid`, local
  `ReflectionProbe` volumes and their existing `IblBakeResult` captures.
  When these are all absent the existing global sky/IBL path is unchanged.
- The Vulkan player uploads *only on probe revision changes*:
  up to 1,024 grid entries, each with nine cosine-convolved SH9 RGB
  irradiance coefficients and one confidence factor; and up to two
  GGX-prefiltered local HDR cubemap captures.
- The live PBR material shader evaluates up to eight nearest SH9 probes per
  shaded surface, interpolating trilinearly in **world metres**. The missing
  confidence fraction goes to the global irradiance. Traced GI alpha takes
  precedence, so baked diffuse and live indirect do not double count.
- Local specular cubemaps are selected per pixel, honoring rectangular
  volume bounds, fade distance, priority, and intensity. Each reflection
  ray is parallax-corrected against the local box; reflection radiance is
  sampled at the material roughness LOD and combined with the shared
  existing BRDF LUT/Fresnel and specular occlusion. Outside these volumes
  the global environment reflection remains in force.
- Fitted SHE glossy reflections remain the environment-only path when no
  local reflection capture contributes. Where a local capture contributes
  the material uses split-sum to blend local and sky specular consistently.
- Scene-defined capture data must be supplied by its owner. The capture
  baker from LP-2A can produce SH9 grids from cubemaps; LP-2B can display
  them, but it does **not** automatically place probes or capture an
  in-game environment.

## Wiring contract

```cpp
dve::player::PlayerRenderView view = ...;
// lifetime: keep these intact through the renderer->render(view) call.
view.localIrradianceGrid = &bakedGrid;
view.localReflectionVolumes = probeVolumes;      // span<const ReflectionProbe>
view.localReflectionCaptures = probeCaptures;    // span<const ReflectionProbeRadiance>
view.localProbeRevision = sceneProbeRevision;    // increment after *every* edit/rebake
```

If an application changes the contents of a grid, repositions a volume, or
replaces the captured cubemap behind a stable pointer without increasing the
revision, GPU resources remain cached and the old capture is displayed.
Handle revision changes before publishing the next frame. Existing
`DVE_SPLIT_RC_EXPERIMENT` and `DVE_GI_DENOISER` variables do not control
local probe shading: the scene references do.

## GPU ABI / performance and safeguards

- `shaders/common/local_light_probes.hlsli` and
  `include/dve/render/local_light_probes_gpu.hpp` share the following
  packing: four float4 header rows, N*10 float4 SH9/visibility-confidence
  rows (x-fastest), then two three-float4 reflection-volume records.
- Frame set 0 adds a read-only storage buffer (binding 3) and two sampled
  local cubemap views (bindings 4/5). All are always bound to a valid
  fallback, avoiding an additional per-material descriptor layout.
- The probe buffer is persistent and uploaded only when a revision changes.
  Captures are persistent GPU cubemaps uploaded at capture changes;
  two selected captures are prioritized by volume priority, then ID.
- Changing probes currently waits for in-flight GPU work before rewriting
  shared resources. That may cause an **edit-time hitch**, not a per-frame
  cost. A multi-buffered residency system remains worthwhile.
- **Critical limitation:** the HLSL sampler uses static per-probe
  confidence supplied by the bake, not a new dynamic visibility ray per
  receiver. Two rooms with a wall between them may leak when a shared
  interpolated grid spans the wall. LP-2C must implement a visibility atlas,
  separated zone grids or a ray-tested receiver validity term. The CPU
  visibility callback from LP-2A is not secretly applied on GPU.
- Grid size is restricted to 1,024 cells; larger grids must be tiled
  before submission. Max two local specular cubemaps resident at once.
- DXC compiles the shader for Vulkan when present. The GTX 1080 has no
  hardware ray tracing requirement: it only samples precomputed SH9 and
  cubemap textures.

## Validation

- `dve_environment_lighting_tests`: GPU layout/packing exact offsets,
  fallback metadata, existing SH9 energy/baking and CPU visibility tests.
- `dve_gpu_local_light_probes_tests`: headless offscreen Vulkan readback
  compares global fallback vs red SH9, a revision change to blue SH9,
  a local amber cubemap reflection, two-capture overlapping-volume
  priority selection, and removal restoring global fallback.
- A test without compiled Vulkan shaders returns an explicit skip; a
  nonexistent Vulkan device is a skip unless `DVE_REQUIRE_VULKAN=1`.
  **Passing CI with skips is not proof of graphics-card execution.**
- Follow-up physical-device acceptance: 1080p 2-room fixture with
  locally baked warm and cool lighting, local metallic material probes,
  incremental bake following destruction, camera movement and static
  fallback; measure GPU pass and total frame times and captured screenshots
  on GTX 1080 against no-probe and global sky-only baselines.

## Next

LP-2C: saved scene-owned probe placement/editor commands, capture scheduling,
visibility and relocation from the voxel grid, tiled residency, dirty-region
invalidations and GPU probe debug views. DN-2: persistent temporal moments
and proper multi-step variance-guided spatial filtering.
