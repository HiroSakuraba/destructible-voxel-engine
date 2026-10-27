# T6 — Rendered image-quality comparisons for BC5 and BC7

## Goal

Compare **the same DVE-rendered material and camera** before/after native texture compression. T5 established BC5 per-texel angular error and KTX2 interoperability. T6 checks whether those texture errors change final beauty and shading-normal outputs, including grazing angles and triplanar mapping.

This first tranche uses DVE's **CPU reference polygon renderer**. It does not claim Vulkan/HLSL GPU-frame parity, in-game frame-rate improvements, or statistically representative artwork quality. The optional BC7 test uses pinned Binomial Basis Universal bc7f analytical encoding and the upstream BC7 block unpacker.

## Test architecture

`dve_texture_render_diff_t6_tests`:

1. Creates three DVE-authored normal-map scenes (flat, ripple, woven) with a textured base color.
2. Uses **`bake_material_textures`**, DVE's actual material baker, to obtain 64×64 RGBA8 normal and color maps.
3. Builds semantic mip chains; encodes native BC5 normal blocks through T3, and optionally BC7 base-color blocks through the pinned Basis bc7f adapter.
4. Independently unpacks those **encoded native blocks** to RGBA8. BC5 Z reconstruction uses the same CPU mirror as the T4 production HLSL shader.
5. Feeds the reference and reconstructed images to the identical CPU `ReferencePolygonRenderer`, holding all nontexture inputs constant.
6. Exercises standard UV and object-space triplanar mapping at overhead and grazing-angle cameras.
7. Compares only pixels covered by identical geometry, asserting compression cannot change the coverage/object ID mask.
8. Writes reference, compressed and amplified difference PPM images, and a `metrics.csv` report (when `DVE_T6_CAPTURE_DIR` is set).

A deliberately sabotaged normal texture must produce **measurable** rendered-image differences. That negative control is important: otherwise, a renderer that ignored textures could make any codec look perfect.

Metrics: covered fragments, changed fragments, display-space RGB mean absolute error, RMSE, PSNR, p95 and max per-pixel channel error, mean and p95 angular difference of world-space shading normals, and separate near-silhouette / interior error averages.

The quality gates are conservative regression limits (not optimized pass/fail thresholds): RGB MAE below 0.15, per-pixel p95 maximum-channel error below 0.40, mean shading-normal deviation below 9°, and p95 normal deviation below 30°. Substantial regressions should fail CI; much lower measured values can inform tighter limits after an artist-supplied corpus is available.

## Running

BC5-only, no upstream Basis dependency:

```sh
cmake -S . -B build-t6 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDVE_BUILD_TESTS=ON -DDVE_BUILD_RHI=ON -DDVE_BUILD_PLAYER=ON \
  -DDVE_BUILD_EDITOR=ON -DDVE_ENABLE_BC7F_ENCODER=OFF
cmake --build build-t6 --target dve_texture_render_diff_t6_tests
DVE_T6_CAPTURE_DIR=build-t6/t6-artifacts ctest --test-dir build-t6 \
  -R '^dve_texture_render_diff_t6_tests$' -V --output-on-failure
```

BC7 codec comparison adds `-DDVE_ENABLE_BC7F_ENCODER=ON`. The source and benchmark remain opt-in in production binaries. `.github/workflows/texture-t6.yml` runs both variants and publishes the PPM images and CSV as CI artifacts.

## Interpretation and limitations

- DVE's reference renderer operates on RGBA8, so the harness **round-trips the actual BC5/BC7 encoded blocks** to decoded RGBA8 before rendering. This measures codec degradation with identical CPU texturing, lighting, and geometry, but **not GPU native-format sampling** or compressed mip LOD/filtering behavior.
- BC5 uses T4's positive-Z shader reconstruction; negative-Z normal maps remain unsupported by BC5 and are explicitly rejected during encoding.
- The normal pass and beauty image are separately measured. BC7 comparisons alter base color; BC5 comparisons alter normal only. Neither compares two changes at once.
- Boundary error bins mean **visible mesh silhouettes**, not a genuine multi-chart UV seam. A true seam suite requires multi-island authored UVs and differential render testing.
- Input maps are procedurally authored but passed through the **real DVE material baker**. These fixtures are not a representative artist art corpus.
- Captured files are binary PPM for portability and can be converted to PNG externally. `difference_x6.ppm` artificially magnifies differences by 6×.
- Accurate quality conclusions and enabling default BC5/BC7 still require actual Vulkan screenshots, UV chart seam scenes, alpha materials, layered materials, multiple LODs, artist-authored assets and a reliable frame-time comparison. These are the next T6 tranches.

### Follow-up acceptance matrix

| Dimension | Current tranche | Future gate |
| --- | --- | --- |
| BC5 normal | Authored→baked→encoded→decoded→CPU render | Native Vulkan BC5 normal shader render |
| BC7 base color | Optional Basis native-block roundtrip→CPU render | Native Vulkan BC7 sRGB sampling and mip LOD |
| UV vs triplanar | Two mapping modes on a plane | Multiple curved surfaces, seam layouts |
| View direction | Overhead and grazing | Wider angles, camera animation, temporal stability |
| Image difference | CSV + PPM + negative control | HDR image metrics, SSIM, artist review |
| Performance | None claimed | Upload/frame-time and streaming p95/p99 |

Keep any automatic compression policy disabled until the GPU and art-corpus gates pass.
