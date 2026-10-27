# Compressed texture loading (T1)

DVE loads single 2D native GPU-compressed images from DDS (BC family) or KTX2 (BC, ETC2/EAC, ASTC LDR), and uploads their mip payloads directly without expanding to RGBA8. This path is separate from `TextureResidencyManager`'s decoded RGBA8 image path and from Basis Universal supercompression/transcoding.

## Formats added in T1

| Format | GPU block | DDS DX10 | KTX2 |
|---|---:|---:|---:|
| BC1 / BC3 / BC5 | 4×4 | Existing | Existing |
| BC4 unsigned | 4×4, 8 B | Yes | Yes |
| BC6H unsigned/signed float | 4×4, 16 B | Yes | Yes |
| BC7 unorm/sRGB | 4×4, 16 B | Yes | Yes |
| ETC2 RGB unorm/sRGB | 4×4, 8 B | No | Yes |
| ETC2 RGBA unorm/sRGB | 4×4, 16 B | No | Yes |
| EAC R11 and RG11 unsigned | 4×4, 8/16 B | No | Yes |
| ASTC LDR 4×4, 6×6 and 8×8 unorm/sRGB | 16 B variable extent | No | Yes |

The Vulkan backend enables BC, ETC2 and ASTC LDR compression feature bits **only if the physical device reports support**, then validates optimal-tiling sampled-image support for the chosen VkFormat. Device formats are not silently replaced with RGBA8. The Null RHI stores exact compressed bytes for deterministic tests, including ASTC 6×6/8×8 edge blocks. Unsupported formats fail explicitly so the asset cooker can choose an alternative.

```cpp
dve::CompressedTextureAsset source;
std::string error;
if (dve::load_compressed_texture("Assets/rock.ktx2", source, &error)) {
    dve::CompressedTextureResource gpuTexture;
    if (dve::upload_compressed_texture(device, source, gpuTexture, &error)) {
        // Bind gpuTexture.view and destroy resource via destroy_compressed_texture.
    }
}
```

The reader still rejects arrays, cubemaps, volume textures, unsupported formats, malformed/truncated mip chains and **any KTX2 supercompression**. It does not decode BC7/ASTC back to pixels or automatically compress images. BC6H is RGB HDR; no alpha. sRGB selections must reflect the texture semantic; normal maps and data maps must not use sRGB. The loader's format table is a deliberately bounded native subset, not every Vulkan block format.

## Separate bc7f encoder proof of concept (T1)

Binomial's Basis Universal transcoder includes the analytical `basist::bc7f::fast_pack_bc7_auto_rgba()` 4×4 RGBA block encoder. The upstream API is **currently internal and may change**. DVE has a separate, default-OFF target; it is not yet part of the asset cooker, editor, or runtime:

```sh
cmake -S . -B build-bc7f -DDVE_ENABLE_BC7F_POC=ON -DDVE_BUILD_TESTS=OFF -DDVE_BUILD_RHI=OFF
cmake --build build-bc7f --target dve_bc7f_poc --parallel 2
./build-bc7f/dve_bc7f_poc
```

The opt-in build fetches `BinomialLLC/basis_universal` pinned to commit `ee771a7f913a3233a252eb6d281b4446f6a3c9f5`, or uses an explicitly specified local `DVE_BASISU_SOURCE_DIR`. It builds only the transcoder module and disables its unrelated KTX2/Zstd support. The module requires `-fno-strict-aliasing` on GCC/Clang. See upstream Apache 2.0 license at https://github.com/BinomialLLC/basis_universal/blob/master/LICENSE ; upstream source remains upstream and is not copied into DVE.

The PoC encodes 4,096 deterministic synthetic color/alpha blocks in analytical and partially analytical modes, double-encodes to test determinism, and emits JSON lines reporting ns/block, PSNR (via upstream block SSE), and a reproducibility checksum. It does not pretend to benchmark DVE texture baking, real photographic assets, GPU upload, or game-frame throughput. End-to-end comparisons with bc7e/astcenc and semantic mipmaps belong in T2/T4.

## Validation

```sh
cmake -S . -B build-t1 -DDVE_BUILD_TESTS=ON -DDVE_BUILD_RHI=ON
cmake --build build-t1 --target dve_compressed_texture_tests dve_texture_t1_format_tests dve_vulkan_texture_t1_format_tests
ctest --test-dir build-t1 -R '^(dve_compressed_texture_tests|dve_texture_t1_format_tests|dve_vulkan_texture_t1_format_tests)$' --output-on-failure
```

The test generates native BC/ETC2/EAC/ASTC KTX2 fixtures plus BC4/BC6H/BC7 DX10 DDS fixtures, checks parser mappings, verifies exact GPU-compressed byte counts, checks Null RHI upload/readback, tests ASTC 8×8 non-multiple dimensions, and rejects truncated/supercompressed inputs. `dve_vulkan_texture_t1_format_tests` uploads two-mip 19×11 textures for every T1 format the Vulkan device reports as sampled and requires byte-exact readback; formats the device does not support must report no capability and fail upload explicitly. It skips without a Vulkan device unless `DVE_REQUIRE_VULKAN` is set. Mesa lavapipe (LLVM 19) here exposes BC but not ETC2/EAC/ASTC, so those only exercise the explicit-unsupported path there.

### Follow-up / T2

1. Connect a pinned, versioned encoder adapter to the baking pipeline; fast bc7f for preview and slower quality presets for packaged assets.
2. Integrate KTX2 Basis/UASTC/XUBC7 transcoding with explicit runtime format negotiation, bounded decode memory and CPU job budgets.
3. Benchmark real DVE material maps; verify normal-map vector renormalization and alpha fringe quality.
4. Move data through the existing texture residency/hot reload pipeline, without changing authoritative voxel/state formats.
