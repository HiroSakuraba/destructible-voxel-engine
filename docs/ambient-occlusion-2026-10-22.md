# Ambient occlusion (22 Oct 2026)

Before this change, AO existed only as two GPU shaders that nothing ran. The CPU renderers that
actually draw had none:
- Fast quality (the `dve_player` default) and the AmbientHemisphere mode lit a crevice like open
  ground;
- polygons used one flat ambient colour everywhere;
- the "Ambient Occlusion" setting was saved but read by nothing.

## One definition, three places

`dve/render/ambient_occlusion.hpp` is the definition; the CPU renderers and the GPU passes use
the same rays and weighting:

- Ray i of n leaves the surface (raised by the shadow bias) along
  `CosineWeightedHemisphereSample(normal, RotatedHammersley2D(i, n, PcgHash(pixel ^ frameSeed)))`.
  The CPU mirrors of `PcgHash`, `Hash2` and the hemisphere sample are bit-for-bit the HLSL's.
- A hit at distance d within the max distance occludes (1 − d / max)². Close geometry counts
  fully and farther geometry fades out, so open ground is not darkened by a cliff far away.
- AO = 1 − mean occlusion.

**New in `RenderEnvironment`:**
- `ambientOcclusionSamples` (default 8, 0 = off, at most 64);
- `ambientOcclusionMaxDistanceMeters` (default 1 m).

They are also in Lua (`AmbientOcclusionSamples`, `AmbientOcclusionMaxDistanceMeters`, and the
environment table) and in saved script state. Saved state uses the two spare slots, so older
saves read back with the defaults.

**`render.ao_quality`** now sets the ray count: off 0, low 4, medium 8, high 16, ultra 32. It
gained an "off" choice and left the not-yet-applied list.

## Where it applies

AO scales ambient light wherever nothing traced already sees occluders:
- **Voxels, AmbientHemisphere mode** (Fast quality): the sky/ground ambient times AO. AO rays see
  the voxels, any `sunOccluders`, and the new `ReferenceVoxelRenderer::occlusionPolygons`.
- **Polygons** (all modes except GI off): the polygon ambient times AO. Rays go to
  `PolygonRenderOptions::occlusionRay` when set, otherwise to the polygons being drawn.
- **Hybrid images** (`render_hybrid_reference`, the CPU player): voxels and polygons occlude each
  other. `HybridOcclusionScene` traces both; the voxels get the polygons as `occlusionPolygons`.
- **Not traced one-bounce GI or radiance cascades:** their own rays already return dim bounce
  light instead of sky in a crevice, and AO on top would darken it twice. `shade_primary.hlsl`
  multiplied AO into the traced GI as well; it no longer does. It still applies AO to the
  AmbientHemisphere ambient and to image-based lighting.

## On the GPU (`dve/render/ambient_occlusion_gpu.hpp`)

`execute_gpu_ambient_occlusion` runs the passes through the RHI on one voxel object:
1. `primary_brickmap_trace` traces the camera rays;
2. `generate_ao_rays` makes n rays per hit;
3. `primary_brickmap_trace` traces those;
4. `resolve_ao` reduces them to one value per pixel.

All four are in one command list with buffer barriers between them, and the AO buffer is read
back. The brickmap goes up through the existing `PackedBrickmapRhiMirror`.
`reference_gpu_ambient_occlusion` is the CPU mirror of the four passes.

**Shader changes:**
- `generate_ao_rays` and `resolve_ao` take the ray count from their constants (a former padding
  word) instead of a fixed 8.

**Compiled shaders:**
- The SPIR-V is checked in under `shaders/compiled/ao`.
- For Vulkan, register classes are shifted apart: b → 0+, t → 16+, u → 32+, all in set 0.
- `SOURCES.sha256` pins the HLSL it was built from; the test fails if the HLSL changes without a
  rebuild. The README has the build commands (Slang, or the DXC equivalent).

The player does not use the GPU passes yet; there is no GPU frame path for them to join.

## Measured

**`tests/test_ambient_occlusion.cpp`** checks against closed forms. Rays leave from the shadow
bias, so walls are effectively 0.15 voxels lower. With a long max distance, AO is the share of
sky a point sees.

| case | CPU | exact |
|---|---|---|
| open ground | 1.00000 | 1 |
| ground 10 from a wall 20 high | 0.7244 | 0.7250 |
| ground 3 from a wall 20 high | 0.5747 | 0.5747 |
| ground 15 from a wall 6 high | 0.9655 | 0.9658 |
| slot 10 wide, 10 deep | 0.4517 | 0.4526 |
| slot 4 wide, 12 deep | 0.1665 | 0.1664 |
| slot 20 wide, 5 deep | 0.9007 | 0.8998 |

The wall cases use ½(1 − x/√(x² + H²)) for the wall's share; the slots use sin(atan((w/2)/H)) for
the open share.

**The real weighting** (max 1 m) beside a tall wall matches a numerical integral of
(1 − d / max)² × cos within 0.0002 at 0.5, 2, 5, 9 and 12 voxels.

**The renderers agree with the exact answer:**
- slot floor (exact 0.2263): voxel AmbientHemisphere × AO 0.2261; traced one-bounce GI with
  black walls 0.2248, so the occlusion AO models is the one the traced GI sees;
- ground beside a wall (exact 0.7250): polygons 0.7248; polygon ground beside a voxel wall
  0.7248; voxel ground beside a polygon wall 0.7248.

**`tests/test_ambient_occlusion_gpu.cpp`, on Mesa's lavapipe (a real Vulkan driver running on the
CPU):**
- **Exact match with the CPU mirror.** 6144 of 6144 primary hits are identical. Per-pixel AO
  matches at every pixel, worst difference 0.0000.
- **Convergence.** Over 32 frames, AO at the bottom of the slot averages 0.4549 (exact 0.4526).
- **Null RHI:** four dispatches, no leaked buffers.
- **Without a Vulkan device** the GPU part is skipped; `DVE_REQUIRE_AO_GPU=1` makes it fail
  instead. Building with `DVE_ENABLE_VULKAN_BUFFER_BACKEND=ON` and installing
  `mesa-vulkan-drivers` is enough to run it.
