# DVE checked-in atmosphere SPIR-V

Compute shaders that build the physical atmosphere's lookup tables on the GPU
(`src/render/atmosphere_gpu.cpp`), compiled from the HLSL so they run on any Vulkan device
(Mesa's lavapipe is enough) without a shader compiler in the build.

Register classes are shifted apart for Vulkan, all in descriptor set 0, as for the AO passes:
`register(bN)` -> binding N, `register(tN)` -> binding 16 + N, `register(uN)` -> binding 32 + N.

Built with Slang 2026.12 through `tools/compile_hlsl_spirv_slang.cpp`
(`-target spirv -profile spirv_1_3 -stage compute -entry main -fvk-b-shift 0 0 -fvk-t-shift 16 0
-fvk-u-shift 32 0`), then checked with `spirv-val --target-env vulkan1.1`. The DXC equivalent:

```sh
dxc -spirv -fspv-target-env=vulkan1.1 -fvk-b-shift 0 0 -fvk-t-shift 16 0 -fvk-u-shift 32 0 \
    -T cs_6_0 -E main -I shaders -Fo shaders/compiled/atmosphere/atmosphere_sky_view.spv shaders/atmosphere_sky_view.hlsl
```

- `atmosphere_transmittance.spv`: 9,096 bytes; SHA-256 `cd30bc72aa47855573eb0f31890cd340659e151d3647d4330049e51d107932d5`
- `atmosphere_multiple_scattering.spv`: 15,428 bytes; SHA-256 `648fa2e085e389dd9b13a07590dac9b0bf9a8552d5f5d0e3eaafd4041ea2cbbc`
- `atmosphere_sky_view.spv`: 15,484 bytes; SHA-256 `39cc0d6248535d3e7b642e62c5a9e9c128eaf5fa70e3d55535205cd265267d23`

`SOURCES.sha256` records the HLSL these were built from (the three shaders and
`common/atmosphere.hlsli`). `dve_atmosphere_gpu_tests` fails if any of them changes without the
SPIR-V being rebuilt and that file updated.
