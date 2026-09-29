# DASHR surface-space heightfield port

DVE now carries the renderer-neutral foundation for **DASHR — Dynamically Animated
Skinned Heightfield Rendering**, described and released by Tom Forsyth in September
2026.

Upstream research/demo:

- https://github.com/tomforsyth1000/DASHR
- `paper/DASHR_Paper.html`

The upstream author explicitly describes the project as a starting point for adapting
the technique into other rendering pipelines and offers a choice of MIT No Attribution
(MIT-0) or public-domain/Unlicense-style terms. DVE uses the MIT-0 grant for provenance.
See `third_party/DASHR-NOTICE.md` and `third_party/DASHR-LICENSE.txt`.

## Why this is a port instead of a vendored demo

The upstream program is a DirectX 11 proof of concept with its own application shell,
asset set, ImGui scaffolding, matrix helpers, texture setup, and demo-specific assumptions.
Those pieces do not belong in DVE.

The DVE port starts from the transferable algorithm:

1. parameterize a deforming polygon surface by UV plus a height coordinate,
2. build a local transform from deformed object space back into that surface space,
3. represent the transform using an object anchor plus inverse differential basis so it
   interpolates more stably than a full affine matrix,
4. measure deformation/stretch in the surface atlas and shorten ray steps in unstable
   regions,
5. march the viewing ray in object space while repeatedly mapping it into surface space,
6. compare the mapped surface-space height against the heightfield,
7. cross UV seams through a destination/teleport map rather than treating seams as
   boundaries, and
8. refine the first crossing to recover a more accurate intersection.

This is substantially different from ordinary parallax occlusion mapping. POM assumes a
single tangent frame and moves through a heightfield locally. DASHR updates the
object-to-surface relationship as the skinned/deformed mesh bends and can explicitly cross
UV seams.

## Implemented in the first port

`include/dve/dashr_surface.hpp` and `src/dashr_surface.cpp` provide the CPU/reference
contract:

- `DashrSurfaceSample`
  - surface UV,
  - object-space anchor for `(u,v,0.5)`,
  - three rows of the inverse non-orthonormal surface basis,
  - U/V deformation ratios.
- `make_dashr_surface_sample`
  - constructs the object-to-surface differential from deformed position, tangent,
    bitangent, and normal vectors.
- `measure_dashr_distortion`
  - central-difference distortion measurement normalized so an identity surface is
    approximately `(1,1)`.
- `dashr_object_to_surface`
  - maps nearby object positions back into `(u,v,height)` and returns a distortion-aware
    ray-step factor.
- `trace_dashr_heightfield`
  - bounded object-space marcher,
  - shell escape,
  - variable safe step size,
  - seam teleport,
  - hit refinement,
  - explicit step/teleport budgets and status codes.

The CPU implementation is intentionally callback-based. Tests can provide synthetic fields;
the GPU implementation will replace those callbacks with deformation, height, and teleport
textures without changing the behavior contract.

`shaders/common/dashr_surface.hlsli` mirrors the transferable per-sample math for HLSL. It
is included by the existing material-mapping reference shader so normal shader validation
also parses and compiles the new helper layer.

## Deliberate differences from the proof of concept

### Stable local-offset representation only

The upstream demo exposes two distortion storage modes. DVE starts with the more stable
representation: inverse basis vectors plus the deformed object-space anchor corresponding
to the surface-space point `(u,v,0.5)`.

This is the representation we want for production because each stored quantity has a direct
local meaning and behaves better under interpolation.

### Central derivative normalization

The DVE distortion helper divides a plus/minus finite difference by `2 * texelStep`, so an
undeformed identity parameterization measures approximately one. This makes the damping
thresholds easier to reason about and test.

### Hard runtime budgets

The proof of concept includes a very large emergency loop cap. DVE makes step count,
refinement count, and seam teleports explicit bounded settings because the live renderer
cannot allow an individual material to create unbounded fragment work.

### No demo-framework dependencies

No Dear ImGui demo code, STB integration, Poly Haven assets, DirectX 11 setup, or upstream
matrix helper is copied into DVE.

## Renderer infrastructure now implemented

The port now includes the renderer-side data path that the live shell will consume.

### Multi-render-target RHI

DVE's graphics-pipeline contract now supports an ordered set of up to four color targets while
retaining the old single-`colorFormat` API for existing callers. Null RHI validates target-count,
dimension, and pipeline/pass compatibility. Vulkan creates matching attachment descriptions,
framebuffers, blend states, and clear values.

This was necessary because one DASHR deformation update should rasterize the mesh once and emit
all four surface-field records, rather than repeating the geometry pass four times.

### Scaled deformation stream

`GpuDashrSurfaceVertex` is intentionally separate from `GpuPolygonVertex`. DVE's ordinary
lighting tangent is normalized; DASHR requires the actual scaled derivatives `dP/du` and
`dP/dv`.

`build_dashr_surface_vertices` reconstructs those derivatives from triangle geometry and UV0,
area-weights them at shared vertices, and measures current stretch/compression against the rest
surface. `DashrSurfaceMeshMirror` keeps the resulting GPU stream in reusable buffers. Passing a
CPU-deformed object-space position for every vertex already gives skeletal/morph systems a correct
reference update route without reallocating the stream every frame.

For a rest surface, the U/V deformation ratios are one. If a pose stretches the object twofold
along the rest U differential, the U ratio is approximately two. Negative or near-zero values
remain visible to the tracer as compression/inversion evidence.

### Four-target UV deformation atlas

`DashrAtlasResources` owns four RGBA16F raw atlas targets and four edge-filled targets.
`record_dashr_atlas_update` rasterizes the scaled deformation stream directly in UV space and
stores:

- target 0: inverse surface basis row 0 + U distortion,
- target 1: inverse surface basis row 1 + V distortion,
- target 2: inverse surface basis row 2 + validity,
- target 3: deformed object-space anchor + validity.

The targets end each update in `ShaderRead` state for the future shell/material pass.

### Bounded edge fill

`dashr_edge_fill.hlsl` performs a deterministic two-pixel nearest-valid dilation around UV
islands. The radius is deliberately small: this pass repairs interpolation support at an island
edge; it must not invent a bridge across unrelated islands.

### Automatic seam teleport map

`cook_dashr_seam_map` groups triangle edges by quantized object-space endpoints. When exactly
two geometric copies represent the same manifold edge but their UV endpoints differ, it emits a
bidirectional teleport band. Open boundaries are ignored and non-manifold coincident groups are
skipped conservatively.

Each seam texel stores the destination UV, a filterable positive seam-region value, and validity.
The destination is inset into the paired triangle rather than placed exactly on its boundary.
The runtime tracer also has a destination-band cooldown: after teleporting it will not immediately
teleport back until it has left the target seam region. This closes a subtle A-to-B-to-A loop that
bilinear filtering can otherwise create.

The atlas resource binds the same seam map with both a linear and a point sampler. The region may
be filtered; the discontinuous destination UV must be point sampled.

## Remaining renderer-facing stage

The remaining major rendering milestone is the live shell/material path.

A DASHR-enabled polygon draw should use conservatively extruded shell geometry. Its fragment
shader will:

1. start from the interpolated surface coordinate,
2. march the camera ray in object space,
3. sample the edge-filled deformation atlas using the previous surface coordinate as the next
   lookup seed,
4. apply distortion damping when necessary,
5. teleport at cooked UV seams,
6. compare against the material height texture,
7. refine the first crossing,
8. discard escaped fragments without writing depth,
9. write depth from the recovered displaced object-space position, and
10. pass the recovered UV/normal into DVE's PBR material evaluation.

The shadow caster must consume the same surface-hit contract so visual depth and shadow depth do
not disagree.

### Current integration blocker

The existing live environment shaders predate production descriptor-set execution and use legacy
global HLSL register ranges while the RHI records several bind groups. The Null-RHI tests validate
orchestration, not actual shader descriptor execution. Before presenting DASHR as a Vulkan-live
material, the shell shader should use an explicit descriptor-set contract (or the live environment
binding scheme should be made explicit for all of its existing resources).

That is an integration issue, not a DASHR-math issue. The deformation atlas, seam map, trace oracle,
and bounded work contracts are now independent of it.

## Integration boundary

DASHR should be a polygon material/rendering capability, not a new geometry type and not a
replacement for voxels.

Recommended enablement:

- polygon or hybrid geometry,
- unique non-overlapping UV atlas for the DASHR surface field,
- tangent/bitangent differential available after deformation,
- bound height texture,
- bounded shell height.

Ordinary polygon materials continue to use DVE's existing mapping path. Voxels continue to
use the voxel renderer.

## Production checks before calling the live path finished

- animated elbow/knee seam case,
- strongly stretched skinning case,
- compression approaching local inversion,
- multiple seam crossings,
- grazing camera rays,
- silhouette edge correctness,
- correct depth against nearby ordinary geometry,
- shadow caster parity,
- motion-vector/temporal-AA behavior,
- atlas memory and bandwidth accounting,
- fragment step histogram,
- maximum-steps fallback visualization,
- stable behavior under morph targets plus skeletal skinning.

The CPU reference added in the first port is the oracle for these later GPU tests.
