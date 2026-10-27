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
has Normal semantics, matches the actual view being bound and uses BC5, linear
RGBA8 or linear BC7 (PreferBc7 normals use the legacy RGB decode). It clears the
BC5 flag for every invalid, missing, RGBA8 and BC7 case; sRGB and other formats
are rejected.

MainMaterialDescriptorTable::bind_resident_normal(assetHash,materialIndex,resident,error)
can replace an already-authored normal texture view with a verified resident
format. It stages a new bind group, then drains in-flight GPU work with
device.wait_idle() (the renderer submits frames asynchronously and every frame
shares the mapping record and the previous bind group), writes the correct
mapping flags and swaps the descriptor. Any failure before the swap leaves the
old descriptor and mapping flags untouched; rejected input never stalls the GPU.
Call it on the render/control thread between frames.

restore_authored_normal(assetHash,materialIndex,error) undoes the override:
the authored view is rebound and the BC5 flag cleared. The host MUST call it
(or invalidate the asset) before destroying, evicting or hot-reloading an
external resident view; the table never owns or destroys external views, and
TextureResidencyManager may evict or replace views during pump().

## Tests

- dve_bc5_shader_mapping_tests: correct view identity, RGBA fallback, bit 4
  preservation and a BC5 round-trip oracle (known normals -> T3 codec -> decode
  -> compared with the source; invalid XY clamp, +Z hemisphere, unit length)
  using the CPU mirror decode_bc5_normal_sample().
- dve_bc5_main_material_binding_tests: Null-RHI material table, BC5 publication,
  replacement by RGBA8, stale flag clearing, bad update rejection (semantic, sRGB,
  unsupported format), wait-idle ordering, BC7 acceptance, restore/undo, flag
  preservation and resource counts.
- dve_vulkan_bc5_normal_decode_tests: real-device (lavapipe/SwiftShader) live
  binding with a BC5 resident, plus, when a SPIR-V capable DXC exists at build
  time, an HLSL compute probe (shaders/bc5_normal_decode_reference.hlsl) that
  samples a real BC5 texture and compares the production decoder with the CPU
  mirror. Without DXC the GPU decode half is skipped.
- dve_bc5_shader_source_contracts: source-level guard for DASHR, UV, triplanar
  projections and the unchanged whiteout blending formulas.
- Existing DVE material mapping, main material table and BC5 tests run alongside.

## Limitations / next batch

- The GPU probe checks decode math, not full-frame lighting: shader source
  contract tests and numerical tests do not equal optical parity screenshots. Validate with DVE baked normal
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
