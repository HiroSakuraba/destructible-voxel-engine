# Batch 6: Layered Surface

Initially built on `162f4d8` (mesh-displacement PR #206), then integrated current
main `20980a4`, including the merged Batch 5 glass PR #205. Texture baking remains
Batch 7; the glass secondary-receiver restrictions remain explicit.

## Artist workflow

1. Select polygon or subdivision objects and open the material browser.
2. Choose the assigned material slot with `[` / `]`, then click **Layers**.
3. Use a starter recipe, or select a compatible leaf preset on the left and click
   **Add selected preset**. Adding an overlay creates an asset-local source slot.
4. The first control selects the current overlay. Scroll over the right column
   to reach the remaining controls. Click numeric controls, type the exact value,
   and press Enter. Escape cancels numeric entry.
5. Add, duplicate, remove, reorder, mute or solo overlays; edit weight, source slot,
   color blend, semantic, mask image/UV/scale/bias, height override/UV/blending,
   opacity policy and source normal strength/mapping/UV transform.
6. Each action immediately changes the assigned slot on every selected target as
   **one undo transaction**. Ctrl+Z / Ctrl+Y undo/redo. Solo is an authored edit;
   undo restores the previous enabled states. Source controls edit a shared leaf
   slot, so every overlay referencing that slot sees the change.
7. Save the scene to retain material definitions, bindings and embedded mask maps.
   The ordinary leaf-preset save/apply path is guarded while editing stacks.

The three original recipes are paint/rust/dirt over metal, worn painted wood,
and dirty/scorched plaster. They create deterministic embedded coverage masks
and independently editable source materials. Recipes replace the chosen slot's
surface settings; unused source slots remain available rather than renumbering
references elsewhere in the asset.

Previews use immutable copies of the complete stack and map dependencies. The
existing bounded worker discards results for superseded generations. Shape
changes retain the source slot and layer dependencies. Sphere/rounded-box
fixtures now supply texture coordinates for mapped previews.

## Renderer implementation

The base remains separate from a maximum of four overlays. Existing asset layer
definitions, bindings, hashes and serialization are reused, with no format bump.
The stable 116-byte material record is unchanged. Main-pass records now contain
the original base definition; masked/textured stacks are evaluated per pixel
instead of resolving uniform layer values before sampling. Existing GPU stacks
may therefore change appearance: roughness/metallic now use the CPU compositor
convention independently of the selected RGB blend mode, and opacity follows its
explicit layer policy.

`build_layered_surface_payload` constructs a portable storage-buffer payload for
the requested stack. Images keep their native dimensions, separately generated
normal/color/data mips, independent UV selectors, wrap modes and min/mag filters.
Repeated image/interpretation/sampler combinations within a stack are deduplicated; differing UV/sampler bindings share typed image texels.
RGBA8 pixels remain packed as one word each. The payload has a conservative
128 MiB per-stack budget and rejects invalid or oversized inputs before descriptor
publication. Main and masked-shadow tables own their payload buffers and release
them during invalidation/clear. Image payloads are currently duplicated between
the main and shadow tables; cross-table sharing is a future memory optimization.

Layer sampling uses explicit normalized texel centers, integer wrap addressing,
bilinear/trilinear filtering and footprint-based LOD in shader code. This adds
one read-only storage-buffer binding and **no hardware samplers** to either pass.
It avoids multiplying sampler descriptors by four overlays. It does add buffer
loads and shader complexity; this is a portability foundation, not a claim that
software filtering is faster than hardware texture sampling.

Color is decoded before interpolation; data/normal maps use their declared data
interpretation. The compositor retains DVE's perceptual-roughness interpolation,
artistic Lerp/Multiply/Additive color modes, additive emission, explicit opacity
policies and height-aware coverage. Height never creates coverage outside a zero
mask. Source normals are evaluated in their mapped frame and composed with RNM
in a common geometric tangent frame. Source procedural recipes evaluate before
composition; the base recipe evaluates after the stack, matching CPU ordering.

The main and CSM caster shaders call the same layer evaluator before alpha
discard. The shadow path therefore includes layer opacity, mask and height
coverage. Reflection contracts identify the new buffer at main set 3/binding 14
and shadow set 3/binding 3. Scalar-record loader offsets are checked individually
by shader contract validation, in addition to C++ record-size assertions.

CPU source evaluation now honors per-binding UV selectors and footprints, mapped
normal orientation and procedural source recipes. CPU texture sampling uses
normalized texel centers and seam-crossing repeat/mirror addressing, matching the
GPU convention. The existing ray-hit evaluator is the shared material foundation
for the future same-mesh UV baker. The layered raster path also reuses its root
channel evaluator; the rest of the rasterizer remains an existing implementation.

## Supported boundary

- Polygon/subdivision authoring; existing CPU semantics remain available elsewhere.
- Base plus four leaf overlays, including dormant-reference validation.
- Opaque/masked final materials; compatible standard-PBR overlay sources.
- Base color, metallic/roughness, normal, emission, opacity, height, mask and
  procedural surface recipes; UV0/UV1 and object/world triplanar source mapping.
- Standard nearest/linear sampler settings represented by PolygonSampler.
- Existing live renderer coordinate/transform conventions; no new nonrigid
  instance normal-transform or animation contract is introduced.

Nested stacks, glass/transmission sources, parallax, legacy voxel normal maps and
detail-map overlays are rejected rather than silently flattened. Layer masks use
the parent material mapping/transform with independent mask UV selection; source
maps use their own material mapping. Mask selection currently cycles existing
asset textures; this batch does not add an image import dialog or paint brush.
Layer stack recipes are saved in scenes/assets, not as independent external-stack
preset files. The panel provides a beauty preview; standalone channel/debug views
remain renderer/test interfaces rather than a new panel preview mode.

RGBA16/HDR image support, physical optical coatings, projection/cage baking,
dynamic mask painting, multi-object atlases and glass secondary texture sampling
remain follow-up work. Baking these stacks will require the same UV, filtering
and tangent conventions; it must not bake illumination into base color.

## Validation

Final verification completed on 24 October 2026 after integrating main `20980a4`.

| Check | Result |
| --- | --- |
| Targeted CPU/editor/RHI suites | 12 passed |
| Canonical shader compilation | 62 SPIR-V and 62 DXIL outputs passed |
| Explicit live shader interface reflection | 9 interfaces passed |
| GPU surface baseline | 192 analytic probes and 3,960 live shaded pixel comparisons passed |
| Focused GPU texture/mapping/filter cases | 101 passed, including tiny-UV ordinary and layered normals |
| GPU layered channels | 5,120 scalar comparisons passed; maximum error 0.00196081 |
| Main/shadow coverage | 28,800 pixel comparisons passed |
| Merged glass regression | 84 optical probes, 9 scenes and 1,455 channel comparisons passed; maximum error 0.00224715 |
| Core-only validation link/run | Passed without the RHI or render-bridge libraries |

The targeted suites are `dve_layered_surface_tests`, `dve_main_material_table_tests`,
`dve_shadow_material_table_tests`, `dve_vulkan_main_material_table_tests`,
`dve_vulkan_shadow_material_table_tests`, `dve_live_environment_renderer_tests`,
`dve_vulkan_live_environment_renderer_tests`, `dve_material_layers_tests`,
`dve_material_mapping_tests`, `dve_material_surfaces_tests`,
`dve_editor_material_presets_tests` and `dve_master_material_tests`.

Tests cover payload layout and rejection, shared typed pixel storage with independent
UV bindings, scene save/reload, editor transactions, numeric input and pointer routing,
all three recipes, source/mask mapping, channel blending, and main/shadow alpha coverage.
The ray-hit regression verifies that an infinitesimal cone retains the same mapped
normal orientation as a finite footprint. Pure stack validation lives in the core
library so editor builds without an RHI can still author and validate stacks.

The final focused GPU rerun additionally exercises tiny UV scales on both ordinary
and layered normal maps. It uses the optional fourth argument `--textures-only`
(after shader-directory and output-directory arguments) to rerun texture/layer cases
without repeating the already-passing analytic and surface-recipe baseline.

GPU execution used software Vulkan (Mesa llvmpipe), not a discrete graphics card.
Observed warm 64×64 frames with upload/readback included were approximately 3–11 ms
for zero to four overlays. These single-sample CPU elapsed measurements are diagnostic;
concurrent load and pipeline creation make them unsuitable as production performance
claims. Cold shader/pipeline setup is substantially more expensive. Hardware profiling
and shader/filtering optimization remain follow-up work.

## Review lessons

Reviewed Codex notes on PR #204 (no findings) and #205 (camera-inside opaque-hit
attenuation and disconnected-volume nesting findings). Those Batch 5 optical
findings are outside this surface-layering change. Here the corresponding discipline is
to validate the full requested stack, use original material records, compare
actual rendered property channels, and test main/shadow discard directly instead
of inferring correctness from a beauty image. Both source and shader manifests
must reflect the final implementation.
