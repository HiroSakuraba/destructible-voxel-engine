# DVE checked-in ambient-occlusion SPIR-V

Compute shaders for the GPU ambient-occlusion passes (`src/render/ambient_occlusion_gpu.cpp`),
compiled from the HLSL sources so the passes can run on any Vulkan device (a software ICD such as
Mesa's lavapipe is enough) without a shader compiler in the build.

Register classes are shifted apart for Vulkan, all in descriptor set 0:
`register(bN)` -> binding N, `register(tN)` -> binding 16 + N, `register(uN)` -> binding 32 + N.

Built with Slang 2026.12 through `tools/compile_hlsl_spirv_slang.cpp`
(`-target spirv -profile spirv_1_3 -stage compute -entry main -fvk-b-shift 0 0 -fvk-t-shift 16 0
-fvk-u-shift 32 0`), then checked with `spirv-val --target-env vulkan1.1`. The DXC equivalent:

```sh
dxc -spirv -fspv-target-env=vulkan1.1 -fvk-b-shift 0 0 -fvk-t-shift 16 0 -fvk-u-shift 32 0 \
    -T cs_6_0 -E main -I shaders -Fo shaders/compiled/ao/resolve_ao.spv shaders/resolve_ao.hlsl
```

- `generate_ao_rays.spv`: 8,748 bytes; SHA-256 `8773bced32b66c9726f35ae1df56e996fbb7ca26edaa2add052547d1417e9f63`
- `primary_brickmap_trace.spv`: 19,152 bytes; SHA-256 `cdbb1ad41151c675585ab55eceaa1ec03b940009e6305200f4fe5f64a20029fb`
- `resolve_ao.spv`: 5,000 bytes; SHA-256 `5a26afb6f9cb6b7121026d2a578ef32363d13ac947a75fc1d450dfb9adfd4bcd`

`SOURCES.sha256` records the HLSL these were built from (the three shaders and their includes).
`dve_ambient_occlusion_gpu_tests` fails if any of them changes without the SPIR-V being rebuilt
and that file updated.
