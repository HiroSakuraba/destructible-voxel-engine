# GPU atmosphere tables (25 Oct 2026)

Stacked on `feature/volumetric-fog`. The physical sky's lookup tables (`feature/atmosphere`) can
now also be built on the GPU. The results match the CPU tables on a real Vulkan device.

## What runs

There are three compute shaders, recorded in one command list with a barrier after each:

| shader | table | reads |
| --- | --- | --- |
| `atmosphere_transmittance.hlsl` | transmittance, 256 × 64 | — |
| `atmosphere_multiple_scattering.hlsl` | multiple scattering, 32 × 32 | transmittance |
| `atmosphere_sky_view.hlsl` | sky view, 192 × 128 | transmittance, multiple scattering |

`shaders/common/atmosphere.hlsli` holds the shared pieces: the media, the phase functions, the
table layouts and the lookups. The host side is `execute_gpu_atmosphere(device, environment,
shaders, tables)` (`dve/render/atmosphere_gpu.hpp`). It packs `GpuAtmosphereConstants`, creates
and destroys its own buffers, and reads the three tables back. The SPIR-V is checked in under
`shaders/compiled/atmosphere`, together with the hashes of the HLSL it was built from.

## Matching the CPU

The shaders use the same quadrature as `src/render/atmosphere.cpp`: the same parameterisations,
step counts, step spacing, direction sets and ground term.

**Float precision.** The CPU computes in double-precision metres. The GPU works in float
kilometres and writes ray-sphere terms as products, (T − r)(T + r) and h(2R + h), instead of
differences of squares. It also carries the observer's altitude separately: r − R in float loses
it near the ground. The horizon angle uses atan2(√(h(2R + h)), R) instead of asin(R/r) near 1.

`dve_atmosphere_gpu_tests` runs on lavapipe (llvmpipe, LLVM 20). Worst relative differences:

| case | transmittance | multiple scattering | sky view |
| --- | --- | --- | --- |
| sun 30° | 8.3e-4 | 1.1e-3 | 1.2e-4 |
| sun 3° | 8.3e-4 | 1.1e-3 | 3.4e-4 |
| hazy (Mie × 8), sun 45° | 2.0e-3 | 1.2e-3 | 1.3e-4 |

The test also checks the SPIR-V hashes, and that the Null RHI sees three dispatches with no
leaked buffers. Without a Vulkan device it skips; `DVE_REQUIRE_ATMOSPHERE_GPU=1` makes it fail
instead.

## Not yet

- **The sky-irradiance table** is still CPU only. It is an integral of the sky view, so it would
  be one more small shader.
- **Nothing consumes the GPU tables yet.** As with AO, the GPU frame doesn't run on a host. When
  it does, the sky, sun and aerial-perspective lookups in `atmosphere.hlsli` are ready for the
  shading shaders.
- **Volumetric fog** remains CPU only.
