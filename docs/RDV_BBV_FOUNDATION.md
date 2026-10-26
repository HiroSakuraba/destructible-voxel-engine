# RDV-1 / RDV-2 — BBV occupancy and toroidal camera clipmap

This is an opt-in foundation for a Raster-Derived Voxel (RDV) lighting/query backend.
It does **not** replace the main rasterizer, the GPU brickmap, the production ray
tracer, or the existing lighting integrations. The first source is DVE's CPU-authoritative
`PackedBrickmapScene`. Raster-derived polygon voxels and GI are RDV-3+ work.

## Source and attribution

Conceptual inspiration: [nagakagachi/instant-rdv-ue](https://github.com/nagakagachi/instant-rdv-ue),
MIT license, copyright 2026 nagakagachi. This implementation is written against DVE's
native layout rather than transplanting the Unreal Engine modules or byte layout.
If upstream source is copied/adapted in later changes, preserve its MIT notice
in `third_party/notices/` and the generated shipping notices.

## RDV-1: occupancy and ray validation

- `BbvBrick`: 16 uint32 words = 512 bit flags = 64 bytes, 8x8x8 voxels.
  Bit number = localX + 8*localY + 64*localZ, matching
  `PackedGpuBrickRecord::occupancy32`. Upstream InstantRDV's subbrick/Morton
  bit numbering is **different**; copying its shader directly is incorrect.
- `BbvClipmap::trace`: bounded CPU fine-voxel DDA against the compact occupancy
  clipmap, with ray/AABB clipping and first-hit face normals. No material decode.
  It is the correctness oracle for the independent shader.
- `shaders/rdv_bbv_trace.hlsl`: independent Shader Model 6.0 compute reference.
  Uses no raytracing or mesh-shader extensions. Declared in the canonical shader
  manifest. `execute_bbv_gpu_trace` dispatches it on DVE's RHI with readback.
  The Null backend validates resource binding and dispatch but does not execute
  shader arithmetic; opt-in Vulkan validation compares actual hits against CPU.
- Native-voxel inputs are ground truth: a mesh depth or a probe cannot clear or
  overwrite a native voxel. No raster-derived occupancy exists yet.

All ray positions, maxima, distances, and grid origin coordinates are **voxel-index
units**, not metres. Converting from render/world metres must use the explicit
metres-per-voxel value. The API intentionally does not infer it from camera transforms.

## RDV-2: camera following and GPU publication

```cpp
using namespace dve::render::rdv;
BbvClipmap clipmap({16, 16, 16}); // 128 fine voxels per axis
BbvRhiMirror mirror(device);

// After PackedBrickmapScene has been brought up to date:
(void)clipmap.recenter_camera(cameraWorldMeters, metresPerVoxel, packedScene);
(void)mirror.publish(clipmap, &error);

// On an authoritative edit, after packedScene.update(object, edits):
std::vector<BrickKey> changed;
for (const auto& edit : edits) changed.push_back(edit.key);
(void)clipmap.refresh(changed, packedScene);
(void)mirror.publish(clipmap, &error);
```

The viewport owns its independent clipmap, buffer and publication lifecycle.
`recenter_camera` floors the world position to brick coordinates; sub-brick
movement changes nothing. The toroidal mapping is:

```
logicalKey = absoluteBrickKey - minimumKey
physicalCoord = positive_mod(logicalKey + toroidalOffset, gridDimensions)
physicalIndex = px + dimX*(py + dimY*pz)
byteAddress = physicalIndex*64
```

On a small camera scroll, the physical GPU slots of overlapping cells are
unchanged. Only newly exposed brick keys are reconstructed from the authoritative
packed scene. On long teleports all displaced slots are replaced, including
clearing previously occupied slots. The resulting 64-byte brick writes are
sorted and coalesced into contiguous `rhi::IDevice::write_buffer` ranges.

GPU publication is synchronous at the current RHI contract boundary; no
asynchronous fence ownership or frame-overlap lifetime guarantees are supplied
by this component. Publication failure retains dirty flags so a later retry can
reconcile the mirror. Full publication is mandatory for a new buffer.
`BbvRhiMirror::readback_matches` provides byte-for-byte resource validation.

A 64x64x64 brick clipmap has 16 MiB occupancy payload. Additional lighting,
material, probe and staging buffers are not counted. The prototype bounds
each axis to 64 bricks. For distant worlds, use camera-relative clipmap keys
and appropriate render-world rebasing; do not insert floating-point camera
coordinates directly into occupancy indexing.

## Tests

`dve_rdv_bbv_tests` covers:
- Whole clipmap first publication, no-op publication and Null-RHI readback
- Negative brick keys, destroyed voxels, nonzero materials
- One-brick toroidal scrolling (only exposed slab written)
- One-brick destruction edit (64-byte write), unrelated edits and teleport clears
- 1,200 seeded rays compared against `raycast_packed_brickmap_hdda`
- Zero-length rays, invalid budgets, validation and resource teardown

Run:
```sh
cmake -S . -B build -DDVE_BUILD_RHI=ON -DDVE_BUILD_TESTS=ON
cmake --build build --target dve_rdv_bbv_tests -j4
ctest --test-dir build -R '^dve_rdv_bbv_tests$' --output-on-failure
./scripts/validate_shaders.sh
```

## Integration limits and next work

1. Run the opt-in native Vulkan shader parity test in a GPU-enabled runner,
   then expand coverage to thousands of randomized rays and Direct3D 12.
2. Add a per-viewport frame owner/call site; current code is an explicit API,
   not a default renderer hook.
3. Support polygon raster-derived surface injection, with separate source masks.
4. Add empty-brick arithmetic skip acceleration *only after* proving identical
   edge/corner hit semantics to DVE's authoritative DDA.
5. Add visible-surface probes, sky visibility and irradiance accumulation.

Do not interpret the CPU oracle and Null-RHI upload checks as proof of GPU
ray-tracing speed or driver correctness. Benchmark frame and dispatch time separately.

### Native shader readback (opt-in)

Compile with DVE's ambient-occlusion SPIR-V descriptor shifts:
```sh
dxc -spirv -fspv-target-env=vulkan1.1 -fvk-b-shift 0 0 \
    -fvk-t-shift 16 0 -fvk-u-shift 32 0 \
    -T cs_6_0 -E main -Fo /tmp/rdv_bbv_trace.spv shaders/rdv_bbv_trace.hlsl
DVE_RDV_TRACE_SPV=/tmp/rdv_bbv_trace.spv ctest --test-dir build \
    -R '^dve_rdv_bbv_tests$' --output-on-failure
```
This opt-in path requires `DVE_ENABLE_VULKAN_BUFFER_BACKEND=ON` and a working
Vulkan device (software ICDs can run shader validation). The test checks two
opposing rays, normals and distances, including a negative-direction ray.
