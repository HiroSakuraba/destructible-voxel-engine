# Wax, skin, bubble and foam material batch

This batch integrates the supplied Tier 2 sheen/iridescence patch into the material system and adds eight usable presets to the existing browser. It targets the CPU reference renderer and material studio. It does not depend on the playable prototype branch.

## Presets and authoring

| Stable ID | Appearance | Model and authored scale |
|---|---|---|
| `dve.wax.paraffin` | Ivory candle wax | Subsurface; 8 mm scatter distance |
| `dve.wax.beeswax` | Warm beeswax | Subsurface; 4 mm scatter distance |
| `dve.skin.light` | Light skin | Subsurface; 1.5 mm scatter distance, 0.7 mm pore repeat |
| `dve.skin.medium` | Medium skin | Same scattering model with darker pigment |
| `dve.skin.deep` | Deep skin | Same scattering model with deeper pigment |
| `dve.bubble.soap-thin` | Soap bubble | 350 nm water film, air interior |
| `dve.bubble.soap-thick` | Soap bubble | 650 nm water film, air interior |
| `dve.foam.dense` | Dense white foam | Opaque effective subsurface medium; 2 mm scatter distance, 1.5 mm surface breakup |

Open the Material Presets panel, select the CPU reference backend, choose a family, and apply to a polygon or voxel selection. Existing shared-slot editing, Undo, favorites, user preset saving and reset-to-origin behavior apply. Appearance parameters preserve gameplay physical properties, just as earlier presets do. These are original artistic defaults, not measured biological, wax or foam fits.

New inspector controls expose scatter distance in metres, RGB scattering albedo, RGB sheen and roughness, thin-film weight, film index of refraction and thickness in nanometres. Existing control IDs and override bits are unchanged; new controls append to the list. Preset requirements append Subsurface, Sheen and Iridescence feature IDs. Subsurface presets declare their feature and require a positive distance. Invalid in-memory sheen and film values are refused before Apply/save.

## Rendering

The supplied patch carries a Charlie sheen lobe and a bounded RGB thin-film evaluator into CPU direct, ambient and sampled BSDF paths. Sheen remains an additive artistic lobe; the supplied opaque-film tint approximation is retained. Neither is claimed to be a complete energy-compensated OpenPBR or glTF material implementation.

The additional soap-film path uses complete thin-film boundary reflectance for transmissive interfaces. A scalar luminance probability chooses reflection or transmission, while RGB sample weights preserve each channel's energy. Transmission uses the complementary reflectance and the existing radiance eta-squared factor. Rough dielectric evaluation and sampling share this probability. Shadow rays and photon boundary transport use the same RGB reflectance. Film weight zero preserves the ordinary dielectric interface. Total internal reflection remains lossless.

Soap presets use a water film with IOR 1.333 over an air substrate with IOR 1. The interior therefore has no glass-sphere bending. Each mesh boundary is treated as one film: use a single closed sphere, not a separately modeled inner and outer wall. Two surface crossings each carry their own film. There is no dynamic film drainage, spatial film-thickness texture, shell thickness simulation or bubble motion.

Existing polygon/voxel subsurface sunlight translucency and front-lit diffusion remain available. Camera/path transport, including the material studio, now reuses the existing random-walk solver for closed Subsurface materials. Each volume encounter selects one RGB channel and reweights by three; its medium uses pigment multiplied by scattering albedo and the metre-authored distance. Entry and internal boundary Fresnel use the material IOR. A walk is bounded to 256 scattering events; the existing independent boundary-reflection cap also applies. Dropped paths terminate, so a difficult highly scattering volume can have truncation bias.

The sampled spatial exit is shaded with a **Lambertian exit approximation**, using next-event estimates for local lights and sun and a cosine continuation with MIS. This does not reproduce the random walk solver's exact outgoing angular distribution. It gives the studio and secondary paths actual spatial scattering rather than an opaque color swatch. The ordinary primary reference surface renderer retains its existing directional-light diffusion/translucency model; this batch does not replace every renderer with a common BSSRDF.

Skin pores and foam breakup reuse the metric procedural surface-detail machinery. Foam is an effective dense scattering surface, not a mesh of individual bubbles. Cellular silhouettes, open wet froth, fluid motion, skin layers, anatomical maps, melting wax and flames are separate work.

## Persistence and compatibility

The Tier 2 foundation extends DMESH through minor 8, DVOX through minor 9, and editor material streams through version 9 when the corresponding features are present. Sheen/film-free records keep the earlier writer paths. Optional `dve.material_sheen` and `dve.material_iridescence` sections carry inline save-game materials. Asset, runtime scene, world-state and material comparison hashes include enabled features. Layer blending includes the added visual fields. Readers reject unsupported future versions and invalid numeric ranges.

GPU upload/HLSL parity for this batch is not implemented. The material browser reports the new presets as unavailable on GPU, rather than presenting them as verified there. The native viewport remains approximate; the CPU studio is the appearance reference. CPU player dispatch still requires acceptance. Thin-film color is a sampled RGB spectral approximation, not a full spectral renderer.

## Reproduction

Build `dve_soft_material_batch_tests`, the material sheen/iridescence suites, material presets, game saves, asset cooker, transport, and subsurface suites. Run them with CTest and `--output-on-failure`.

Generate reviewable data assets and studio thumbnails with:

```sh
dve_material_preset_gallery /tmp/dve-soft-gallery --soft
```

The gallery contains eight `.dvepreset` files, eight BMP/PPM previews and `index.html`. The checked-in assets under `assets/materials/presets` are generated by the same catalog and writer.

The regression suite covers complementary RGB film energy, air-interior straight transmission, thickness color changes, zero/disabled film, total internal reflection, rough eval/sample PDF agreement, preset/DMESH/DVOX round trips, authoring validation and override reset, GPU capability refusal, metric subsurface distance and skin pigment reaching camera transport.

The uploaded full-suite log's eleven failures are not proof of unrelated baseline failures: it does not include their assertions or stack traces. All eleven were rebuilt and rerun here: both network suites and all nine editor/NURBS/modeling suites passed in 4.47 seconds. The earlier failures did not reproduce; their original root cause is still unproven. The full repository suite was not rerun.

## References

- [Khronos glTF material overview](https://www.khronos.org/gltf/pbr/): distinction between thin-film interference and volume attenuation.
- [KHR_materials_iridescence](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_iridescence): film IOR and thickness semantics. DVE's artistic opaque approximation is not claimed to conform to this extension.
- [Physically Based Rendering, subsurface diffusion](https://www.pbr-book.org/3ed-2018/Light_Transport_II_Volume_Rendering/Subsurface_Scattering_Using_the_Diffusion_Equation): spatial light transport and approximation limits.
- Existing DVE `subsurface-random-walk-2026-10-22.md` and `subsurface-diffusion-2026-10-22.md` document the reused solvers.

## Local validation record

The fresh Release build with reference physics and Lua disabled passed all 16 focused CTest suites in 40.04 seconds: soft material batch, material presets, sheen, iridescence, game save, asset cooker, material transport, subsurface translucency, three material baking suites, scene lighting, subsurface random walk/diffusion and caustic photon/map suites. All 62 shader contracts and validator self-tests passed. The eight presets were generated and their studio contact sheet inspected. Studio sampling is 128 paths for subsurface and 32 for other materials; the 64-pixel previews remain noisy smoke tests, not production image acceptance. GPU/windowed acceptance was not performed.

The separate rerun of all eleven failures from the uploaded log passed, bringing the local validation total to 27 passing suites. This is not a claim that the full repository suite ran.
