# LP-1 / DN-1 — local light probes and optional one-bounce GI denoising

**Status:** Initial CPU-reference implementation plus opt-in player shader filter. This
is not yet a production GPU light-probe renderer or a multi-level SVGF pipeline.
Do not advertise improved GTX 1080 performance without the device measurements below.

## What existed before this tranche

- `ReflectionProbe`, legacy `select_reflection_probes`, and
  `box_project_reflection_direction` in `environment_lighting.cpp`.
- Cubemap GGX convolution, split-sum, and optional SHE environment reflection.
- One-bounce half-resolution traced GI temporal accumulation and depth-aware
  upsample in the Vulkan player; Split RC has a separate full-resolution
  receiver-aware upsample and is *not* processed by the new DN-1 path.

## LP-1A: bounded local specular reflection probes

- `select_reflection_probe_mix` returns at most two local probe weights
  **plus explicit environment weight**.
- Only probes whose boxes actually contain the shading point participate.
  Influence fades towards the box faces over `blendDistance`, and the
  unused fraction goes to global environment light. This avoids stretching
  one room's reflection across a wall into the next room.
- Highest-priority valid zones supersede lower-priority zones; equal-priority
  zones mix deterministically by influence, then probe ID.
- `sample_reflection_probe_radiance` computes a parallax-corrected
  direction per resident capture and mixes roughness-prefiltered **specular
  radiance**, with the sky fallback if a capture is not resident. It
  deliberately does not add diffuse GI or apply Fresnel/BRDF a second time.
- The older `select_reflection_probes` API is preserved for compatibility.
- No editor probe authoring, GPU cubemap array, automatic scene capture,
  saved revision binding, or runtime Vulkan probe sample shader yet.

## LP-1B: diffuse SH9 irradiance grid reference

- `IrradianceProbeGrid` is a regular 3D XYZ array with world origin,
  cell spacing, per-probe **cosine-convolved** real SH9 RGB irradiance and
  capture confidence.
- `sample_irradiance_probe_grid` samples the eight nearest entries
  trilinearly, skips invalid confidence and negative fitted irradiance,
  and returns **preweighted irradiance** with a [0,1] coverage fraction.
- The caller may compose `probe.irradiance + (1 - coverage)*skyIrradiance`;
  do not normalize away missing support, and do not add baked diffuse
  on top of traced/RC diffuse without an explicit ownership switch.
- Validation caps dimensions and total probe count; runtime sampling does
  not rescan every probe in the entire grid.
- No probe capture/baking path or Vulkan 3D-array upload is implemented.

## DN-1: optional motion-aware GI history clamp and spatial filter

Set `DVE_GI_DENOISER=1` when using one-bounce Vulkan GI
(`--renderer vulkan --gi bounce`). Omitting the environment variable keeps
the existing temporal/upsampling shader behavior.

- The temporal fragment pass clamps reprojected radiance to a valid
  current-frame 3x3 neighborhood, rejecting large camera-depth differences
  before using the bounds. Previous-frame camera-distance validation remains.
- The upsample pass enables a one-level 5x5 separable wavelet-kernel style
  cross-bilateral resolve: edge-stopping uses current depth, reconstructed
  surface plane, and linear HDR luminance differences. It falls back to
  the original 3x3 depth-only upsample if the 5x5 neighborhood has no
  compatible support.
- The existing GPU constants ABI is unchanged. `gGiTemporal.y` encodes
  0=legacy/history valid; 1=legacy/invalid; 2=DN-1/history valid;
  3=DN-1/invalid. The denoiser is a `bounce`-path option; not a
  Split RC or material specular filter.
- **Not SVGF:** no temporal first/second luminance moments, variance,
  confidence/sample age, motion-vector buffer, material/object ID or
  multi-level à-trous passes. Temporal clamp cannot guarantee correct
  motion reprojection for animated geometry. This pass has extra depth
  fetches and may be slower on GTX 1080. No temporal performance proof.

## Acceptance matrix

1. `dve_environment_lighting_tests`: priority, sky residual, box-edge
   fade, missing capture, constant cubemap, SH9 trilinear coverage and
   invalid bounds.
2. `dve_v2293_shader_gi_fixes_tests`: opt-in flag, default behavior,
   temporal depth rejection, plane stopping source contract.
3. Compile the updated HLSL with DXC on both Vulkan and Windows.
4. Hardware A/B: capture camera pans, thin-wall/foreground edges, sky,
   emissive toggles and teleport/resizes, and compare with DN-1 off.
5. Record GPU total frame time, temporal and upsample pass GPU timestamps,
   visual noise, ghosting, edge leak count and VRAM at 1080p GTX 1080.
   Avoid comparing CPU wall-clock timing as if it were isolated GPU pass time.

## Future LP-2 / DN-2

- **LP-2:** source capture, saved probe volumes and revisions, editor
  placement/priority control, border/visibility validation, Vulkan cubemap
  residency, specular BRDF integration and global fallback.
- **LP-2 GI:** bake cosine-convolved SH9 to 3D grid, visibility/relocation,
  dirty-region invalidation, guarded dynamic GI ownership. The SH9 CPU
  reference here is not already this runtime.
- **DN-2:** per-pixel first/second luminance moments + age, disocclusion
  reset using normal/material IDs, variance-adaptive multi-step à-trous
  feedback with measured quality/cost tradeoffs, plus independent
  screen- and world-probe histories.

## Reference designs

- Schied et al., *Spatiotemporal Variance-Guided Filtering*, HPG 2017:
  https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/
- Unity manual, local reflection-probe selection / skybox residual:
  https://docs.unity.com/en-us/engine/6000.6/manual/lighting-overview/lighting/reflection-probes/introduction
