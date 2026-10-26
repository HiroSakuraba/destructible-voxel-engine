# Batch 7: static material texture baking

This change is stacked on Batch 6, PR #208. It bakes the shared unlit material
surface evaluator into PNG8 maps on a static polygon mesh. It does not bake
path-traced lighting, AO, photon density, shadows or tonemapping.

## Editor workflow

1. Select one visible, unlocked polygon object. Apply subdivision before baking;
   this batch deliberately does not bake a subdivision control cage or NURBS display.
   Apply Live Mirror as well. An enabled Exact CAD Source can be exported, but
   must be removed before replacing its generated polygon geometry.
2. Open the material browser, choose the assigned slot, and click **Layers**.
   The bake controls work for both leaf materials and layered surfaces.
3. Choose UV0 or UV1, resolution, samples per axis, padding, and optional separate
   opacity. UVs must form unique, nondegenerate charts within [0,1]. Use the UV
   editor to unwrap/pack first; baking never silently repacks either UV stream.
4. **Validate bake target** checks the source, dependencies, continuous UV overlap,
   and estimated memory. Coverage and pixel gutter checks run during baking.
5. **Bake / export new bundle** saves under the open project's
   `Materials/Bakes/object-ID-slot-SLOT-TIMESTAMP/`. **Bake / export / apply copy**
   also assigns a new derived material as one undo transaction. Esc cancels the
   background task. A changed scene never receives a stale result.
6. The bundle contains the original `source.dmesh`, map PNGs, `provenance.json`,
   and a self-contained `baked.dmesh`. The derived mesh embeds its maps, retains
   both UV streams and original source slots, and duplicates only target corners
   with white vertex colors. Persistent geometric, corner, face and edge identities
   are retained; new attribute entries receive fresh identities. Base color/alpha therefore are not multiplied by
   baked vertex colors a second time. Separate opacity is an export aid; the
   derived material already reads base-map alpha and does not bind it twice.
7. Undo restores the exact editable source. For later rebaking, import the
   bundle's **source.dmesh**, change it, then export a new bundle. The source
   file is authoritative: assigning an old material slot to the derived geometry
   cannot recover the original target vertex colors.

A completed export remains on disk if scene application is cancelled or rejected
as stale after publication. Export is an independent, useful artifact, not an
undoable file operation. Existing output directories are never replaced. Owned
staging directories are cleaned on failure/cancellation; process termination can
leave an unreferenced `.tmp-N` directory, never a partially published bundle.

## Command line

```
dve_bake_material source.dmesh NEW_BUNDLE_DIR --material 0 --uv 1 \
  --resolution 1024 --samples 2 --padding 8 --maps 31 --budget-mib 512
```

`--maps` is a bitmask: base color=1, normal=2, metallic/roughness=4, emission=8,
opacity=16. Default 15 supplies every map required by the derived material.
Map-only subsets export source, maps and provenance with `derived_mesh: null`;
they cannot be applied as an incomplete material. `--meters-per-unit` defaults
to 1; the CLI freezes the source at the identity pose. Editor jobs freeze the
selected object's current world pose with one coordinate unit equal to a metre.
World-mapped recipes therefore become pose-specific. SIGINT/SIGTERM request
cooperative cancellation. Unknown/malformed options, failed validation and
existing destinations return failure.

## Texture contract

| Map | Encoding | Channels |
| --- | --- | --- |
| Base color | sRGB RGB; linear alpha | Evaluated color and opacity, including vertex colors |
| Normal | Linear | DVE derivative tangent frame, +Y; normalized, packed into [0,1] |
| Metallic/roughness | Linear | R=1 (no AO), G=perceptual roughness, B=metallic, A=1 |
| Emission | sRGB, LDR | Unlit emission RGB; A=1 |
| Opacity (optional) | Linear | Evaluated opacity replicated to RGB; A=1 |

The baker uses full destination-texel world differentials for source mip and
procedural filtering; supersampling refines coverage and channel integration.
RGB is averaged in linear light and encoded after averaging. Normals are averaged
as vectors and normalized; existing renderer mip residency supplies distinct
sRGB, linear scalar, and renormalized normal mip chains on reload. Normal output
uses DVE's mapped derivative frame; it is **not advertised as an external
MikkTSpace bake**, although cooked geometry may retain its MikkTSpace tangent stream.
The CPU raster normal decoder now shares the ray/bake mapped-frame implementation,
including UV1 and object-triplanar orientation.

Continuous triangle clipping rejects mirrored and subpixel overlaps independently
of resolution. Shared edges are permitted. Charts are connected by matching
position/UV/normal edges with compatible handedness. Rasterization uses a top-left
rule, rejects distinct charts sharing a destination texel, and requires every
triangle to cover at least one supersample. Multi-source dilation retains chart
identity and rejects insufficient pixel gutters. `safe_mip_levels` is only a
padding-based advisory; arbitrary coarse mips will mix charts. Atlas edges use
clamp sampling. Padding does not guarantee safe sampling at every mip or arbitrary
anisotropy.

## Provenance and limits

The provenance records a versioned recipe key, source hash, map encodings, target
UV, resolution, sampling, padding, scale and frozen pose. The source content hash
covers embedded images, bindings, samplers, layers, material values, geometry and
UVs. Changing these dependencies or bake settings invalidates application of an
in-memory result. The hash is an invalidation key, not a cryptographic digest.
There is no automatic filesystem watcher or in-place rebake; explicit source
reload and a new bundle are the persistent rebake workflow.

Opaque/masked standard PBR surfaces and an existing uniform clear coat are
supported. Layers obey Batch 6's leaf-source restrictions. Transmission, parallax
and distance-faded detail maps are rejected. Selected RGB maps exceeding PNG8's
LDR range fail instead of clipping. No HDR/16-bit, UDIM, high-to-low projection,
cage, animated/skinned target, displacement/height export or lighting bake is
introduced. Source normal/height recipes follow the shared ray surface evaluator;
optional raster height-bump render settings are not part of this bake contract.

Work is bounded by resolution 8..4096, samples 1..4 per axis, 100,000 target
triangles, explicit memory budget, and UV-validation cell/pair budgets. The editor
exposes 256..2048 resolutions and a 512 MiB default budget. Expensive preflight,
sampling, mip generation and export run on the existing editor job worker.

The Batch 6 review's referenced-source edit bug is also fixed: layer edits now
validate every dependent stack before one atomic command is added to history.
Regression coverage exercises the source edit that previously introduced nesting.
The previous displacement review also informed the explicit modifier boundary and
attribute-split modeling-identity validation/undo regression.

## Validation

`dve_material_texture_bake_tests` covers channel encodings, no double vertex
color/alpha, derived reimport, source preservation, continuous/subpixel overlap,
UV bounds/degeneracy, resource bounds, LDR failure, map subsets, UV1 normal
handedness under a frozen pose, derivative-driven minification, chart padding,
cancellation, no-overwrite export, async editor apply/undo/redo, stale scene
rejection, scene reload and CLI success/failure. Existing layered-surface,
texture/mapping and shader contract checks guard shared behavior.
