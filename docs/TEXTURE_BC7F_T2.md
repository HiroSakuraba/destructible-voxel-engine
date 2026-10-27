# T2 — BC7 texture residency with the pinned Basis bc7f encoder

## Implemented

- Off by default: DVE_ENABLE_BC7F_ENCODER; normal builds do not fetch or link Basis.
- Worker-side TextureBc7Encoder hook invoked by TextureResidencyManager::enqueue, not during the GPU pump.
- Separate dve_texture_bc7f static target uses BinomialLLC/basis_universal at commit ee771a7f913a3233a252eb6d281b4446f6a3c9f5.
- Edge-clamped 4×4 block construction, native BC7 mips, analytical and partially analytical encoder presets.
- GPU upload chooses BC7 sRGB for base color/emissive or linear BC7 for data only when supported; otherwise RGBA8 with fallback telemetry.
- Resident bytes reflect actual selected GPU format; pending staging budget stays conservatively calculated from RGBA8.
- Hot reload preserves old resources on failure and invalidates references on successful replacement. Compression and mip-policy changes defeat source-hash no-op detection.
- Encoder failures, thrown exceptions, and malformed block payloads fall back safely to RGBA8.

## Usage

Link dve_texture_bc7f in the asset preparation host. Construct TextureResidencyManager(device, budget, make_basis_bc7f_encoder()); enqueue TextureUploadRequest with PreferBc7 from an asset-preparation worker. Call pump(uploadByteBudget) on the RHI thread. Material bakes already produce PolygonImage data suitable for these requests.

## Build

Configure: cmake -S . -B build-t2 -G Ninja -DDVE_BUILD_TESTS=ON -DDVE_BUILD_RHI=ON -DDVE_ENABLE_BC7F_ENCODER=ON -DDVE_BUILD_ASSET_COOKER=OFF -DDVE_BUILD_BENCHMARK=OFF -DDVE_BUILD_ASSET_PIPELINE_TESTS=OFF
Build: cmake --build build-t2 --target dve_texture_bc7f_residency_tests dve_texture_residency_tests
Test: ctest --test-dir build-t2 -R '^(dve_texture_bc7f_residency_tests|dve_texture_residency_tests)$' --output-on-failure

## Boundaries

- This is a functional optional residency path, not yet a UI-level material bake switch, background scheduling framework, save-format change, or universal Basis/KTX2 transcoder.
- The callback is synchronous on the enqueue caller and can be expensive for large textures: call from a worker. bc7f access is serialized pending upstream thread-safety evidence.
- BC5 normal compression remains deferred. Normal maps using PreferBc5ForNormals retain the RGBA8 fallback; explicit PreferBc7 uses linear data mode.
- Default compiler and runtime paths remain unchanged. The Basis low-level API is internal and may change; preserve Apache-2.0 license notices on redistributed upstream binaries.
- Next: a semantic quality corpus, BC5 normal maps, packaged KTX2/Basis assets, budgeted background transcode and load-time p95/p99 profiling.