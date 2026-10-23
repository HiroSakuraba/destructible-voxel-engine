# CPU material transport

The Cornell glass sphere previously received opaque surface shading. CPU reference/hybrid rendering now follows camera-visible reflected and refracted rays for materials with `transmission > 0`, and exact mirror rays for full metals at zero roughness. Optical materials activate this path even when diffuse GI is off or the fast reference preset disables ordinary bounces.

## Authoring

| Material | Suggested values |
| --- | --- |
| Clear solid glass | `transmission=1`, `indexOfRefraction=1.5`, `roughness=0`, `baseColor.w=1`, opaque coverage |
| Frosted glass | Same, with `roughness=0.25` or higher |
| Polished metal | `metallic=1`, `roughness=0`, RGB base color supplies normal-incidence reflectivity |
| Rough metal | `metallic=1`, nonzero roughness |
| Coverage transparency | Translucent blend mode and alpha; this controls surface coverage rather than the IOR |
| Colored solid glass | Glass plus `attenuationColor` and positive `attenuationDistanceMeters` |

The built-in Glass entry keeps its material ID but now defaults to the clear-solid values above. Existing saved material definitions are not migrated: alpha-only glass needs its transmission enabled explicitly. Transmission mixes a dielectric interface with the existing opaque diffuse/specular/clear-coat response. A transmissive interface uses IOR-derived Fresnel reflectivity; the existing `specular` control applies to the opaque response. Absorption color is a transmittance after the authored attenuation distance, not a diffuse surface tint.

Use closed meshes with outward winding and appropriate smooth vertex normals for solid glass. Voxel glass retains voxel face normals. Contiguous same-material voxel cells form one volume, without internal cell Fresnel interfaces. The medium stack restores the enclosing material after leaving nested volumes; initial voxel containment and first outward polygon hits support cameras inside a medium.

## Transport

- Exact dielectric Fresnel branching, Snell refraction and total internal reflection.
- GGX visible-normal sampling for rough reflection and transmission, with matched mixture PDFs and emitter MIS. Zero-roughness glass and full metal have delta paths.
- Radiance-mode squared-IOR transport and IOR-compensated roulette; Beer–Lambert attenuation on medium segments.
- Stochastic alpha coverage, masked cutoffs and polygon opacity texture/layer sampling along continuation paths.
- Shared continuation code for camera optics and diffuse/specular secondary bounces. Front raster-resolved material and normal parameters are preserved.
- Geometric-side ray offsets and refined voxel face positions avoid DDA accumulation putting spawned lighting rays behind a distant wall. The legacy voxel tracer remains unchanged.

Local light shadow visibility follows straight segments through dielectric interfaces with Fresnel attenuation and absorption. This is an approximation: it does not bend shadow rays toward a light or solve focused area/emissive caustics. Sun caustics still use the separate existing photon subsystem. Fog's emitter visibility retains its previous behavior. GPU material/transport parity is not part of this change.

Paths have finite interaction/distance/coverage budgets. Camera optics reserve at least four interactions (twelve with ordinary bounce tracing disabled) so disabling diffuse GI does not remove glass. Higher samples reduce Monte Carlo noise; no denoiser or firefly clamp is added. Secondary RGB/roughness texture filtering remains limited to material constants; opacity coverage is texture-aware. Optical camera AOVs classify radiance by the first sampled transmission/specular/diffuse branch, including light evaluated at that vertex, rather than claiming an exact per-vertex lobe separation; their sum equals beauty.

## Cornell regression

`tests/data/cornell_glass_lights.scene` is the supplied text fixture. Its sphere record carries IOR but no transmission/roughness. The dedicated regression loader explicitly constructs clear glass from that record. This small loader is not a general engine scene importer; it supports the fixture's wall/block, sphere, camera and ceiling-emitter records. Resolution and sample count are explicit CLI arguments.

```sh
cmake --build out/materials --target dve_cornell_material_preview dve_material_transport_tests
out/materials/dve_cornell_material_preview tests/data/cornell_glass_lights.scene out/materials/glass.ppm 192 128 glass
out/materials/dve_cornell_material_preview tests/data/cornell_glass_lights.scene out/materials/rough.ppm 128 64 rough
out/materials/dve_cornell_material_preview tests/data/cornell_glass_lights.scene out/materials/metal.ppm 128 64 metal
```

Modes also include `mirror` and `opaque`. An optional final directory argument writes floating-point AOVs. The loader checks finite HDR output and AOV reconstruction. CTest includes a small Cornell smoke render.

## Validation

Material transport tests cover Fresnel entry/exit, total internal reflection, equal IOR, rough dielectric sample/evaluation PDFs and energy bounds, metal reflectivity and roughness spread, closed-slab throughput including internal reflections, RGB absorption, alpha-zero coverage, contiguous voxel volumes, camera-inside attenuation, nested media, distant voxel face precision, and automatic glass rendering with diffuse GI disabled. Existing lighting energy/MIS, caustics, hybrid rendering, legacy voxel tracer, fog, save/script, player, shading and render-pass suites provide regression coverage.

Implementation references: [PBRT dielectric BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF) and [microfacet roughness](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory).

Local validation used GCC 13.3 in Release mode with editor/audio/Vulkan disabled, 16 selected CTest suites, and the editor material-optics suite linked separately with the asset pipeline and editor material library. A focused ASan/UBSan executable instruments the material test, BSDF, scene-lighting and optical-scene translation units while linking the other renderer/core archives from Release; leak detection is disabled. This is not a full sanitized editor or GPU run.
