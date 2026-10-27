# T5: Real DVE material-bake BC5 quality and independent KTX2 validation

## Motivation

T4/PR #245 (merged) added the format-aware BC5 shader and host-side normal-view flagging.
T5 validates **real output from DVE's material texture baker**, not just an analytic BC5
fixture. It also takes the optional Khronos libktx exporter from an unverified build
option to an independently exercised interoperability test (when its CI job passes).

## DVE-baked normal quality

`dve_material_bake_bc5_quality_tests` makes three editable DVE polygon planes with
authored 32x32 normal maps representing flat, ripple and woven texture patterns.
It calls `bake_material_textures` with normal-only 64x64 output and 2x2 coverage
sampling, then `generate_semantic_texture_mips` and the T3 BC5 encoder.
The test first checks that the bake really carried the authored pattern (flat stays within 1 degree
of +Z, ripple and woven tilt by more than 10 degrees), so a baker that ignored the normal map cannot pass.
It then verifies deterministic encoded mips, per-mip mean angular error below
3 degrees (observed 1.54 at worst), worst texel angular deviation below 4 degrees (observed 1.68), at least about
half the RGBA8 footprint, and exact byte accounting in the real DVE Null RHI.
The compressed output is decoded using the T4 shader's CPU mirror
`decode_bc5_normal_sample` to catch a mismatch between encode and shader semantics.
Quality thresholds are regression limits, **not** perceptual equivalence or
measured Vulkan frame parity. Patterns are procedurally authored input maps but
the textures under test are produced by the real DVE material baker.

## Independent Khronos KTX2 interoperability

`dve_texture_ktx2_export_tests` calls DVE's optional exporter, then validates the file two ways that do
not use DVE's reader:

- raw bytes against the KTX 2.0 specification: identifier, `vkFormat`, `typeSize`, pixel size, face/layer
  counts, `levelCount`, `supercompressionScheme == 0`, DFD placement and size, DFD colour model and transfer
  function (**sRGB vs linear**), level-index byte lengths against block-padded sizes computed by hand from the
  block dimensions (BC 4x4, ASTC 6x6 / 8x8; 1x1 and 2x2 tails still occupy a whole block), 16-byte alignment and
  smallest-mip-first storage;
- the real `ktxTexture2_CreateFromMemory` reader: format, dimensions, layout, no transcoding, colour model,
  transfer function, per-level image size and offset, and a `memcmp` of every compressed mip.

It also checks deterministic file bytes, a roundtrip through the DVE native KTX2 reader, and that malformed
mips and truncated files are rejected by DVE and libktx. Mip counts are derived from the texture size
(complete chains, plus one deliberately shorter chain): libktx refuses a chain longer than
`floor(log2(max(w,h)))+1`, so `encode_native_ktx2` now rejects it with a descriptive error, and the test
asserts that. libktx error codes are included in the exporter's messages.

`.github/workflows/texture-t5.yml` builds pinned Khronos KTX-Software v4.4.2
with its own CMake build, installs libktx (the tag is resolved and compared with the pinned commit
`4d6fc70eaf62ad0558e63e8d97eb9766118327a6`; submodules are not fetched because the only one is the unused
conformance suite; the installed library is cached per commit), then compiles and runs this real
interoperability test. This is a separate job from the editor-bake quality
job; either can fail independently. Optional libktx remains OFF in ordinary builds.

## Required outcomes before production enablement

1. T5 real-bake quality test passes in GitHub CI.
2. The pinned upstream libktx job compiles and independent KTX2 roundtrip passes.
3. Live Vulkan pixel/shader parity remains covered by T4 and still requires
   genuine hardware or a software Vulkan ICD; CPU/Null RHI results are not
   proof of visual indistinguishability.
4. Source format-aware BC5 remains opt-in: authored assets, compressed
   residency and lifetime management are not automatically changed by T5.

### Reproduction

Build DVE with `DVE_BUILD_EDITOR=ON`, `DVE_BUILD_RHI=ON`, and `DVE_BUILD_TESTS=ON`,
then run `ctest -R dve_material_bake_bc5_quality_tests -V`.
For KTX2, build/install Khronos KTX-Software v4.4.2, configure
`DVE_ENABLE_KTX2_EXPORT=ON` with `DVE_KTX_INCLUDE_DIR` and `DVE_KTX_LIBRARY`
pointed at the installation and run `ctest -R dve_texture_ktx2_export_tests -V`.

## Remaining

- Compare against artist-provided rough, skin, fabric and metallic materials;
  these fixtures exercise the real bake pipeline but are not production art.
- Add a full Vulkan rendered-difference image test (including UV seams and
  triplanar projection under grazing light) before default-on BC5.
- KTX2 Basis supercompression / XUASTC / XUBC7 transcoding is outside T5.