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

## Next renderer-facing stage

The mathematical core is not yet the complete live feature. The next port stage should add
four GPU resources/passes.

### 1. UV-space deformation atlas

For each DASHR-enabled deforming mesh, rasterize the current skinned surface in UV space.
The atlas stores:

- inverse surface basis row 0,
- inverse surface basis row 1,
- inverse surface basis row 2,
- deformed object-space anchor,
- U/V distortion ratios.

A practical DVE format is four RGBA16F render targets initially. We can reduce bandwidth
later after profiling.

The vertex stage must use the same deformed/skinned position and differential basis as the
normal polygon pass. The atlas is generated after pose/morph evaluation and before the
material pass that consumes it.

### 2. Edge fill

UV islands leave uncovered texels around their borders. Add a small deterministic edge-fill
pass so filtered atlas reads near a seam still resolve to a nearby valid surface sample.

DVE should keep the fill radius explicit and bounded. It should never silently bridge large
unrelated UV gaps.

### 3. Seam teleport map

Cook a static map containing:

- a filtered signed distance or validity measure near a seam,
- a point-sampled destination UV on the adjacent island.

The ray marcher samples the distance continuously. Only after it enters the teleport region
does it point-sample the destination coordinate. Keeping those two semantics separate avoids
interpolating discontinuous destinations.

### 4. Live shell material pass

A DASHR-enabled polygon draw uses conservatively extruded shell geometry. The fragment
shader:

1. starts from the interpolated surface coordinate,
2. marches the camera ray in object space,
3. samples the deformation atlas using the previous surface coordinate as the next lookup
   seed,
4. applies distortion damping when necessary,
5. teleports at UV seams,
6. compares against the material height texture,
7. refines the hit,
8. discards escaped fragments without writing depth, and
9. shades the recovered surface point using DVE's existing PBR material path.

The result must write the correct displaced depth. Shadows should use the same surface-hit
contract rather than the current flat shell depth.

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
