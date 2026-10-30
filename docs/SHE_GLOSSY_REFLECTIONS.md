# Spherical Harmonic Exponential (SHE) glossy reflections for DVE

## Integration status — PR #269

This PR adds a working, optional source-level path for fitting, storing, uploading and
evaluating Spherical Harmonic Exponential environment reflections. The default remains
the legacy split-sum IBL renderer, and **actual frame time, visual parity and VRAM
savings have not yet been demonstrated on hardware**.

Enable it in the Vulkan player:

```text
dve_player --renderer vulkan --ibl she --sky colors
dve_player --renderer vulkan --ibl she --sky physical
dve_player --renderer vulkan --ibl split-sum
```

The live Vulkan material shader uses SHE *only* for supported glossy roughness
when the fit has been published. While a fit is pending, or if the fit fails or the
sky changes, the shader uses the existing split-sum cubemap and BRDF LUT. The
material's clear-coat lobe is selected independently.

## Source and reference implementation

Silvennoinen, Sloan, Iwanicki and Nowrouzezahrai, *Spherical Harmonic
Exponentials for Efficient Glossy Reflections*, Computer Graphics Forum
44(8), HPG 2025. DOI https://doi.org/10.1111/cgf.70219

MIT-licensed external D3D12 reference:
https://github.com/Avelina9X/SphericalHarmonicExponentials

DVE's code is an independent implementation. The external reference uses the
same exp(-l(l+1)alpha/2) roughness factors, and confirms the 24 P non-DC,
8 Q non-DC + 1 DC factorization (33 total). We store the equivalent 25 P
including DC plus 8 Q without DC.

## Architecture

1. `src/spherical_harmonic_exponentials.cpp` samples the source
   EnvironmentCube, constructs GGX base (Fresnel=1) reference values, and
   solves the 33-column log-linear least-squares problem per RGB channel.
   Uses height-correlated Smith masking and a small regularization term.
2. `include/dve/spherical_harmonic_exponentials.hpp` provides the CPU
   reference API and a 208-byte RGB FP16 coefficient format (99 half
   coefficients plus five padding halfs).
3. `shaders/common/spherical_harmonic_exponentials.hlsli` reconstructs
   from 52 uint32 coefficient words using ordinary FP32 operations. The
   Vulkan resource carries an additional marker word and padding, so the
   current **GPU allocation is 224 bytes**, not 208 bytes.
4. `EnvironmentLightingAsset` contains an optional `sheProbe` plus a
   content hash of its *source radiance*. `.dveibl` v1.1 saves the source
   hash and 208 packed coefficient bytes following the existing resources.
   Legacy v1.0 assets remain readable and writer output remains v1.0
   when no SHE payload is present. A changed source invalidates its fit.
5. `src/render/environment_lighting_gpu.cpp` binds an always-valid,
   read-only storage buffer at environment set 2, binding 3. Coefficient
   updates write to the same buffer and preserve descriptor/PSO handles.
   A zero validity marker selects split-sum unconditionally. Resource
   publication must be synchronized with in-flight GPU reads.
6. `shaders/live_ibl_material_ps.hlsl` evaluates SHE's directional base
   integral and replaces the specular half of the existing IBL result,
   leaving diffuse irradiance, indirect diffuse GI, AO, sun, shadows,
   and environmental scaling unchanged. Fresnel remains the existing
   LUT ratio (F0*A+B)/(A+B). Material normal and view directions are
   rotated together into sky space before SH evaluation.
7. `src/player/vulkan_player_renderer.cpp` makes the player opt-in
   asynchronous: scene sky changes invalidate the old GPU marker; a
   background CPU fit on a *copied source cube* is published only after
   its source hash matches the current sky and the renderer has waited
   for outstanding GPU work. Failed fits leave split-sum active and
   do not immediately restart.

## Quality and scope limitations

- Uses GGX microfacet `alpha = perceptualRoughness^2`.
  SHE is enabled only when `alpha >= 0.25`, corresponding to perceptual
  roughness `>= 0.5`. Sharper reflections and clear coat use split-sum.
- The current fit is a CPU Monte Carlo approximation; it does not reproduce
  the paper's GPU fitting/reduction/solver or exposure-calibration stages.
  The player uses 768 directional training points * 64 GGX samples.
  Higher quality offline bakes are possible with `SheBakeSettings`.
- Synchronous `bake_environment_she(asset,settings)` is exposed for
  asset-cooker integrations, with `write_dveibl` / `read_dveibl`.
- DVE **still allocates its traditional cubemap and BRDF LUT**, because
  they are needed for sharp surfaces and as an always-safe fallback.
  Therefore this PR **does not yield a measured VRAM saving** despite
  compact coefficient storage. The paper's ~200x figure refers to a
  particular comparison, not a guarantee for DVE.
- The GPU path currently fits one global sky environment at a time.
  Local/box-projected reflection probes are CPU contract structures,
  not an integrated multi-probe GPU renderer. Multi-probe interpolation
  requires its own residency, binding and blending design.
- SH evaluation involves more arithmetic and buffer reads than one
  prefiltered cubemap lookup; the 1080 GPU could be slower. No GPU
  optimization should be claimed without instrumentation.
- The CPU fitter and shader use FP32 accumulation for GTX 1080
  compatibility; native FP16 arithmetic is not assumed or required.
- No claim yet that the selected fitting resolution beats split-sum in
  every HDR scene. Highlights can be biased by the log-space fit.

## Tests

- `dve_spherical_harmonic_exponentials_tests`: synthetic exact
  coefficient fitting, half packing/unpacking, constant HDR probe baking,
  alpha threshold fallback, CPU override.
- `dve_production_environment_lighting_tests`: v1.0 and v1.1
  round-trip, changed-source rejection, NullDevice coefficient upload
  marker, enabled/disabled state and resource cleanup.
- `dve_vulkan_environment_lighting_tests`: read-only GPU coefficient
  upload/readback and marker lifetime on software Vulkan.
- Existing `dve_vulkan_live_environment_renderer_tests` and player
  builds are needed to confirm shader reflection/pipeline layout validity.

## Hardware acceptance gates (not yet measured)

On the user's GTX 1080 at 1080p: compare `--ibl she` to
`--ibl split-sum` on bright HDR scenes and a Cornell Box-like material
sweep. Record linear HDR MAE/MSE and highlight clips against a high
sample-count GGX reference; repeat over rotating camera and all alpha
levels around the 0.25 boundary. Record median/p95/p99 GPU duration,
frame time while fitting, GPU memory allocated, descriptor/buffer counts
and energy differences in metals and clear coats. Report cold/warm sky
switch, fitting CPU time, stale fit avoidance and visual flicker.
The quality gate is **comparison against a reference**, not appearance
alone; if a scene regresses, keep split-sum as the default.

## Follow-up candidates after hardware evidence

- Avoid sampling the old cubemap when SHE is selected (currently the
  shader computes a delta against the old path for correctness).
- Improve coefficient fitting and brightness compensation against the
  original reference.
- Optional cooked HDRI/box-probe arrays with residency/streaming and
  spatial blending; preserve local parallax corrections and fallbacks.
- Retire cubemap specular data only if a scene's materials, roughness
  ranges and fallback policy permit doing so safely.

