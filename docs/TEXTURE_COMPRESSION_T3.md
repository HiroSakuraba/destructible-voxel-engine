# T3 — BC5 normal compression, texture quality evidence and native packaging

T3 stacks on T2 PR #243; T2 is still open. The new code is independently reviewable and does not require a merge of #243.

## BC5 native blocks

Added `normal_bc5_codec` in DVE's render bridge. It uses the standard BC5 layout (two 8-byte BC4_UNORM blocks per 4x4 tile), deterministic endpoint/selector encoding, clamped partial tiles, semantic normal-map mips, exact 16-byte tile sizes, and a reference decoder.

Negative-Z tangent normals are rejected rather than silently corrupted. The decoder reconstructs Z from X/Y using the positive hemisphere and provides mean angular-error measurements. This codec does not require Basis Universal.

## Residency

Added an optional BC5 encoder callback to TextureResidencyManager. For Normal + PreferBc5ForNormals, a valid encoded mip chain is uploaded as native BC5 only if the GPU supports it. Existing no-codec behavior remains RGBA8 fallback; encode failures and unsupported GPU formats also retain RGBA8. Upload byte budgeting and resident bytes are based on the selected format. Existing BC7 hooks and call sites remain compatible.

**Important shader caveat:** DVE's existing normal-map shaders expect three encoded channels. A native BC5 normal map must use a matching shader variant which reconstructs positive Z from sampled RG. T3 does not yet auto-switch materials to such a variant; don't bind newly encoded BC5 to those existing shaders unchanged.

## Packaging and benchmarks

Implemented deterministic DDS DX10 writer for already encoded BC1/3/4/5/6H/7 with exact mip-chain validation. Added an optional native KTX2 writer through Khronos libktx, default off and requiring an installed libktx; it avoids hand-writing a potentially invalid KTX Data Format Descriptor. KTX2 universal/Basis supercompression and transcode remain future work.

Added a benchmark using representative **synthetic** flat/wavy/woven normals, or a caller-supplied RGBA8 normal texture file from a DVE material bake. It reports encoded bytes, time, mean angular reconstruction error. Do not interpret it as a measured game-frame improvement.

## Tests

- `dve_texture_bc5_tests`: deterministic codec, odd-sized mip blocks, angular error, negative-Z fallback, native Null-RHI residency, generation invalidation, no-codec fallback, resource cleanup.
- `dve_texture_native_cook_tests`: DDS export/import byte-for-byte across BC5/BC6H/BC7, invalid mip rejection, Null RHI upload and ASTC rejection by DDS.
- Optional `dve_texture_ktx2_export_tests` when using libktx: native BC5/BC7/ASTC file roundtrip against DVE parser.
- Dedicated GitHub workflow executes native tests and synthetic quality bench, without optional external dependencies.

## Remaining

- Shader variant and binding metadata for BC5 normal Z reconstruction;
- libktx optional export requires an installed libktx in CI to independently validate full KTX2 interoperability;
- Real DVE art corpus, BC7 vs BC5 cross-quality comparison and Basis XUASTC/XUBC7 transcoding;
- Offline texture baker/cooker packaging handoff and `.dvepak` manifest integration.

Main/default editor path still uses RGBA8 unless a host explicitly supplies and selects BC5 encoding.