# T4 — BC5 Normal Textures in Live and DASHR Shaders

T3 introduced an optional two-channel BC5 normal-map encoder. T4 completes the
necessary **format-aware decode** for live pixel shader sampling and adds a
transactional descriptor-table rebinding interface to keep the format flag in
sync with the actually bound GPU view.

## GPU contract

- The mapping record's layout and size are unchanged. Bit 4 of mappingFlags
  is kGpuMaterialMappingNormalBc5 (C++) / kMaterialFlagNormalBc5 (HLSL).
- If the bit is clear, original RGB normal texture sampling is unchanged.
- If set, BC5_UNORM provides X and Y from sampled RG. Convert to [-1,1],
  reconstruct positive Z = sqrt(max(0,1 - x*x - y*y)), renormalize and pass
  the resulting vector back to the existing derivative/triplanar normal logic.
- Both UV and triplanar projections are handled in live_material_sampling.hlsli;
  DASHR's shell PBR shader uses the same decoder before applying its derivative frame.
- The positive-hemisphere representation matches T3's negative-Z rejection.
- Detail normal, voxel/triplanar-specific material channels and independent
  shader passes still need separate BC5 policy decisions; this bit applies
  ONLY to the main normalTexture channel.

## Safe host binding

Normal-map compression *preference* cannot authorize shader decoding. The
TextureResidencyManager may fall back from BC5 to RGBA8. The helper
configure_bound_normal_format() verifies that the supplied ResidentTexture
has Normal semantics, matches the actual view being bound and uses BC5 or
RGBA8. It clears the BC5 flag for all invalid, missing and RGBA8 cases.

MainMaterialDescriptorTable::bind_resident_normal(assetHash,materialIndex,resident,error)
can replace an already-authored normal texture view with either verified
resident format. It stages a new bind group, writes the correct mapping flags,
then replaces the descriptor. Invalid resident input leaves the old descriptor
intact. For optimal safety the host must call this on the render/control thread
between frames, and MUST retain the external resident view until rebound or
descriptor invalidation. No automatic destruction or lifetime transfer occurs.

## Tests

- dve_bc5_shader_mapping_tests: correct view identity, RGBA fallback, bit 4
  preservation and BC5 decode numerical oracle.
- dve_bc5_main_material_binding_tests: Null-RHI material table, BC5 publication,
  replacement by RGBA8, stale flag clearing, bad update rejection, resource counts.
- dve_bc5_shader_source_contracts: source-level guard for DASHR, UV, triplanar
  projections and the unchanged whiteout blending formulas.
- Existing DVE material mapping, main material table and BC5 tests run alongside.

## Limitations / next batch

- Shader source contract tests and numerical reference tests do not equal
  optical parity screenshots on a Vulkan GPU. Validate with DVE baked normal
  maps and differential renders before enabling BC5 as an editor default.
- The host must explicitly opt into BC5 and call bind_resident_normal for a
  material with an authored normal channel; there is no automatic texture-table
  relinking or source image replacement.
- Independent libktx KTX2 export validation remains separate. Khronos's
  stable 4.4.2 implementation uses a complete CMake project rather than the
  lib-only layout of the later 5.0 pre-release. This batch does not switch
  DVE to prerelease libktx or claim KTX2 conformance.

## Implementation links

- shaders/common/material_mapping.hlsli
- shaders/common/live_material_sampling.hlsli
- shaders/dashr_shell_pbr_ps.hlsl
- src/render/main_material_table.cpp
- tests/test_bc5_main_material_binding.cpp
