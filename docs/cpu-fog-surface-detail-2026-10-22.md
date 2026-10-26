# CPU fog and surface detail: four batches, one PR

This change builds on main at `1b5aad0bf2d880490cb2c3680889a9febea1d66a`
(including the existing distance fog, compositing passes, and voxel normal maps).
It adds CPU rendering features; it does not replace or undo the voxel normal-map work.

## Batch 1: correctness and integration

`apply_cpu_fog` integrates transformed Gabor volumes along the camera ray, clipped
at opaque scene depth. It combines all overlapping extinction/source terms before
compositing, rather than alpha-compositing whole volume objects in submission order.
The same pass includes existing exponential distance fog, with its surface-only sky
policy. Local volumes can appear against the sky. Integration is in linear HDR;
every light AOV is attenuated and the additional radiance goes into the fog AOV.
Depth, material IDs and picking IDs are untouched by fog.

The CPU player accepts optional `PlayerRenderView::fogVolumes` and `fogSettings`.
It converts the player's metre-space volumes/lights into the surface renderer's
voxel units and supplies scene-shadow queries against voxels and polygons. The
existing distance-fog path is retained for views with no local media. The player
owns a bounded density cache and an optional quantized lighting cache.

Detail normals now combine in a common tangent frame and transform back to world
space once. Zero-strength detail preserves the base normal. Steep/POM traces use
the authored height reference plane and include the final shell endpoint. Binary
refinement only runs after a crossing has been bracketed. The result reports a
hit, depth and validity, enabling explicit fallback diagnostics.

## Batch 2: quality and stability

Texture LOD uses perspective-correct quotient-rule derivatives at each fragment
instead of one triangle-area estimate. Sampling blends adjacent mip levels.
Base/detail transforms affect the footprints; mapped triplanar color/scalar
sampling uses position derivatives. Mip caches retain separate normal and sRGB
variants when a source image is used in those roles. Normal mips renormalize the
average vector; sRGB mips average linear radiance. Odd-sized textures retain their
last row/column. Variant payloads count toward the existing mip-cache byte budget.

Fog uses midpoint sampling by default. Optional adaptive step hints derive from
primitive scales/frequencies and instance scale. Quality presets bound the work.
A 2x/4x reduced-resolution option uses depth-gated interpolation and retraces
pixels whose neighbourhood crosses a depth/sky discontinuity. Jitter is deterministic
for pixel/frame/segment. Optional stationary-camera history resets for camera,
resolution, media hash/placement, settings, lighting, or scene-revision changes;
a changed depth rejects that pixel's history. Unhashed media cannot reuse history.
Camera motion resets history: this is not motion-vector reprojection.

## Batch 3: lighting and depth

Optional single scattering evaluates sky illumination, a directional sun, local
point lights with a finite range, and a Henyey–Greenstein phase function. Optional
volume-shadow marches and a scene-transmittance callback attenuate incident light.
The player's default callback treats its existing hybrid occluders as opaque.
A custom callback can supply transmissive visibility. Callbacks must be thread-safe
when multiple workers are enabled, and callers must advance `sceneRevision` when
external shadow-caster/callback state changes.

`CpuSurfaceDetailSettings` adds opt-in height-derived bump normals, bounded sun
self-shadowing through the height field, and explicit `reliefDepthMeters`.
Corrected depth is tested before writing beauty, AOVs or IDs, including when a
recessed surface falls behind an ordinary mesh. The depth AOV follows the same
correction. Relief retains base-mesh raster coverage; it does not generate new
silhouettes, geometry, physics, BVH shadow casters or collision. Bump/relief and
height self-shadowing operate on UV materials. They do not enable triplanar POM.

## Batch 4: CPU performance and diagnostics

`CpuFogDensityCache` owns a BVH of primitive support bounds. Point queries reject
irrelevant supports; ray queries merge conservative occupied leaf intervals and
skip empty gaps. The cache defaults to 262,144 resident primitives and falls back
to the original evaluator for oversized/unhashed assets. Entries own their data;
no borrowed asset pointer survives in the cache. Preparation/eviction occurs before
workers start, and worker queries are read-only.

Rows run in deterministic eight-row jobs on 1–16 workers. Optional spatial light
transmittance caching snaps shadow origins to a configurable cell grid, with a
65,536-entry bound and the same full scene/media/settings key. This approximation
is disabled by default (`lightCacheCellMeters == 0`). History/cache objects belong
to one viewport and cannot be used by concurrent render calls.

There are at most 32 volumes and 32 point lights per frame. The primary step target
is `maximumSteps`, plus mandatory support-boundary samples (at most
`maximumSteps + 129` primary integration segments per ray). Excess BVH intervals
fall back to whole-volume bounds. Shadow steps have a separate 1–64 bound.
Statistics expose rays, integration steps, primitive evaluations, BVH visits,
shadow work, cache hits, early termination, undersampled rays, edge retraces and
history rejection. Polygon statistics expose bump/shadow samples, failed height
traces and corrected-depth writes; the existing parallax sample heatmap remains.

Parallax now accepts a screen footprint/sample budget and conservative height
bounds. Constant-height maps solve in one sample; other maps skip the empty part
of the shell outside their global min/max range. This uses the existing mip cache
rather than introducing a separate cone-map asset format. Cone stepping remains a
possible later optimization: it needs a conservative spatial representation and
validation at bilinear/wrap boundaries, rather than treating averaged mips as
height bounds.

## Minimal CPU player use

```cpp
std::vector<dve::render::CpuFogVolume> volumes;
volumes.push_back({&immutableGaborAsset, worldTransformMetres});
view.fogVolumes = volumes; // borrowed until render(view) returns
view.fogSettings = dve::render::cpu_fog_quality_settings(
    dve::GaborVolumeQuality::High);
view.fogSettings.threadCount = 4;
view.fogSettings.sceneRevision = currentSceneRevision;
view.surfaceDetail.heightBumpStrength = 0.02f;
view.surfaceDetail.parallaxSelfShadow = true;
// Optional physical depth, separate from the material's UV heightScale:
view.surfaceDetail.reliefDepthMeters = 0.01f;
renderer->render(view, &error);
```

These are CPU renderer APIs. This PR does not add serialized editor controls,
replace the native editor's schematic volume proxies, automatically populate
GameWorld media from the editor scene, or claim GPU/DASHR feature parity. An
application supplies the media to the player's view. Existing asset/save formats
remain compatible. Full editor/media authoring wiring is a separate integration.

## Validation and measurement

Dedicated suites cover analytic homogeneous attenuation, occluding walls, overlapping
media/order invariance, unit conversion, AOV reconstruction, invalid input,
stationary history rejection, serial/parallel equality, depth-aware upsampling,
phase/point lights/shadows/cache invalidation, accelerated density vs original
kernels, semantic/odd-sized mips, analytic height gradients, parallax bounds/budgets,
neutral detail under rotation, and corrected depth against another mesh in both
draw orders. The player suite also renders supplied local media and checks its
observable frame/counters and disabled fallback.

Local validation passed 14 focused/adjacent headless suites. The three new suites
also passed AddressSanitizer + UndefinedBehaviorSanitizer with the material
mapping, polygon renderer, Gabor kernel, fog integration and density-cache
translation units instrumented; other dependency archives were uninstrumented
and leak detection was disabled. No windowed artist acceptance run was performed.

The reproducible `dve_cpu_fog_bench` target compares a 675-primitive sparse scene
at 96x64, full resolution, no history, no jitter or light caching. One Release
GCC 13.3 run in this environment measured:

| Mode | CPU ms | Rays | Integration segments | Primitive evaluations |
|---|---:|---:|---:|---:|
| Linear evaluator, one worker | 1175.32 | 6144 | 132900 | 89707500 |
| BVH, four workers | 11.48 | 6144 | 116432 | 390372 |

RGB root-mean-square difference: 0.00009221; maximum channel difference: 0.00097098.
The acceleration changes midpoint interval boundaries slightly. This is a synthetic
scene measurement, not a frame-rate guarantee or an isolated four-thread speedup.

Build/run with a headless RHI configuration:

```sh
cmake --build build --target dve_cpu_fog_tests dve_cpu_fog_quality_tests \
  dve_cpu_surface_detail_tests dve_cpu_fog_bench dve_player_runtime_tests
ctest --test-dir build -R 'dve_cpu_(fog|surface_detail).*tests' --output-on-failure
build/dve_cpu_fog_bench optional-preview.ppm
```

References for the numerical choices:

- Physically Based Rendering, [Transmittance](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance).
- NVIDIA GPU Gems 3, [Relaxed Cone Stepping for Relief Mapping](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping).
