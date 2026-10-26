# Batch 9: bake UVs, selected-object queues, AO and lighting

Batch 9 extends the persistent recipe workflow from Batch 8. Baking is an offline
CPU job. This batch adds no shader ABI or HLSL changes.

## Artist workflow

Open Materials > Layers and choose the bake controls:

- **Bake UVs**: retain an existing unique UV0/UV1 layout, or generate a separate
  UV1 atlas. Atlas generation evaluates original source UV0 and UV1 before
  assigning the output UV1; it never edits the source snapshot.
- **Bake slots**: current slot, saved explicit subset, or all assigned slots.
  Selected slots share texture images, but retain separate derived material slots
  for residual properties such as coat, alpha cutoff and double-sidedness.
- **Minimum texels/m**: zero fits the requested size. A positive density can grow
  the output resolution, up to 4096. Preflight reports actual resolution, packed
  surface utilization and minimum texels per metre. Unattainable density is an
  error, not an unannounced reduction in quality.
- **AO** and **Lighting** add separate output maps. Choose ray samples, AO range,
  lighting bounce count and RGBM range. AO does not multiply the base-color map.
- **Validate bake target** runs UV/material/memory/work preflight in the worker.
  Pixel coverage and chart gutter checks remain authoritative during baking.
- Select up to 64 objects and Bake / export or Bake / export / apply. The queue
  records a result for each object; unsupported or locked targets do not prevent
  other targets from finishing. The Queue report contains paths and errors.

A single-object explicit output path retains its previous meaning. A multi-object
path is a parent for fresh `object-ID` bundles. Outputs never overwrite existing
bundles. Applying successful objects produces one undo step per object. Queue
cancellation leaves scene assignments unchanged; independent bundles already
published by completed items remain on disk. There is no all-or-nothing directory
transaction across multiple objects.

## Generated UV contract

The first implementation uses one projected chart per triangle, with deterministic
height-ordered shelf packing. Flattening uses the longest geometric edge and the
triangle's actual altitude; charts share a world-metric scale. Positive object
scale and meters-per-unit participate in density calculations. Each chart has
padding plus one additional pixel of border. Continuous UV overlap, raster
coverage and gutter validation still run after preparation.

This is deliberately conservative. Dense triangle meshes may waste atlas area or
exceed the allowed gutter/triangle budget. No seam editor, chart relaxation,
curved-surface unwrap, chart rotation optimization or xatlas-quality packing is
claimed. Tiny/sliver triangles with no covered supersample fail with an actionable
resolution/repack error. Mirrored or overlapping source UVs are allowed because
only the destination charts must be unique. Existing-UV mode continues to reject
degenerate or overlapping destination UVs.

## AO

`ambient-occlusion.png` stores linear geometric visibility in RGB and alpha 1.
Each covered surface sample emits deterministic cosine-weighted hemisphere rays
from the geometric receiving normal. A hit at distance d within the AO radius R
contributes `(1 - d/R)^2` occlusion; misses contribute zero. The map is one minus
the mean occlusion. Bias and distance are in metres. Normal maps do not change
geometric AO.

AO traces geometry, including masked/transmissive triangles as geometric blockers;
it does not integrate alpha transmission or participating media. This avoids
misrepresenting an optical transmission estimate as geometric AO. AO is exported
and inspectable but is not automatically bound to a runtime AO material channel.

## Diffuse lighting

`diffuse-lighting-rgbm.png` contains view-independent diffuse illumination for a
unit-white receiver (irradiance divided by pi), using geometric receiver normals.
It includes shadowed sun, point/spot/area/candle lights, emissive mesh sources,
hemispherical environment and optional bounded secondary transport through the
existing shared CPU scene-lighting integrator. Source surface colors and materials
participate at secondary hits. It is not a specular/reflection or camera-radiance
bake. Exposure, tone mapping, bloom and fog are not baked.

RGBM preserves a configurable finite linear range in PNG8:

```
linear_lighting = (RGB / 255) * (A / 255) * lighting_range
```

RGB values are linear data, not sRGB. M is rounded upward to the next 8-bit value.
Values exceeding the range fail the bake rather than clipping. RGBM interpolation
and mip filtering require decoding before averaging; ordinary sRGB or linear
RGBA interpolation is not a valid lightmap filtering contract. A future runtime
lightmap consumer must implement that policy. This batch exports/persists and
inspects the result; it does not auto-bind static lighting alongside dynamic
lighting, avoiding double illumination.

The light rig is captured at simulation time zero for reproducible candle flicker.
The editor imports its Rendering settings and enabled visible light components.
Bake-specific bounces are retained, with a maximum of eight; editor controls cycle
zero through four. Zero bounces still includes directly visible environment;
positive bounces use the shared CPU indirect estimator. Monte Carlo noise remains.
No denoiser or focused-caustic quality guarantee is added.

Visible neighbouring polygon displays are copied into immutable snapshots. Voxel
objects are converted through the existing bounded polygon conversion. The target
is traced from its editable source, and neighbouring bake-associated objects use
their editable source representation. This keeps a queue from changing its own
lighting inputs when results are applied. Live Mirror neighbours and unsupported
visible text/volume geometry produce explicit errors instead of disappearing from
the lighting scene. Hidden geometry is excluded; visible locked neighbours still
occlude and illuminate. Light component transforms are world transforms.

## Persistence, inspection and stale data

Recipe metadata version 2 adds atlas/slot/ray/lighting settings and neighbouring
mesh/pose snapshots. Version 1 recipes remain readable. Scene format 14 remains
sufficient because recipes are stored as independent versioned records. Old
preset/prefab integrity hashes retain the historical signature for old-style
recipes. Scene-owned source/baked snapshots and neighbours remain available after
removing export bundles. Runtime export still uses the assigned mesh and carries
no editor recipe files.

Each new export contains `source.dmesh`, optional `baked.dmesh`, selected PNGs,
`provenance.json`, `settings.txt`, `scene.txt` and fixed-name `scene-N.dmesh`
neighbour snapshots. Pose, source, settings, object scale, seed, ray counts, RGBM
range and scene hashes are recorded. The recipe is hashed for scene/prefab history.
The saved neighbour snapshots support deterministic standalone replay; an editor
rebake captures the current scene and rig.

Existing source/result switching, geometry identity preservation, stale-job
rejection and cancellation guards remain. Click the saved status row to explicitly
check neighbouring geometry and the current rig; lightweight periodic status still
checks source/settings/pose/scale. There is no new filesystem watcher or continuous
background conversion of all scene geometry. Changes during work reject scene
application. Exports remain independent of application.

Inspection adds **AO output map** and **Diffuse lighting output map** to the
channel cycle. These are UV-space output maps, not camera-space material renders.
The source half is a checker because it has no saved baked output. Lighting is
RGBM-decoded and display-mapped for inspection; the underlying PNG remains linear
RGBM data. The original eight material comparison channels remain available.

## CLI

```
dve_bake_material source.dmesh NEW_DIR --atlas 1 --all-materials 1 \
  --resolution 512 --padding 8 --maps 127 --ray-samples 16 --ao-distance 1 \
  --bounces 2 --lighting-range 16

dve_bake_material OLD_DIR/source.dmesh NEW_DIR \
  --settings OLD_DIR/settings.txt --scene OLD_DIR
```

Options are processed in order, so later arguments can explicitly override a
settings snapshot. `--materials 0,2` selects a sorted unique subset; `--all-materials
1` selects each object's assigned slots. Maps: base color 1, normal 2,
metallic/roughness 4, emission 8, opacity 16, AO 32, diffuse lighting 64. Default
remains 15, preserving the ordinary unlit bake workflow. Partial map exports do
not manufacture a complete material association.

## Resource limits

- Resolution 8..4096; 1..4 raster samples per axis; padding <=64 pixels.
- Up to 100,000 target triangles; up to 64 queued objects.
- Ray samples 1..256; <=8 secondary bounces and <=16 local-light samples.
- Conservative 64-million ray-work-unit preflight; quality settings that exceed it
  fail before raster work rather than silently reducing samples.
- Explicit per-bake memory budget (default 512 MiB), bounded UV validation work.
- Neighbour capture <=250,000 triangles and 256 MiB; combined queue snapshot
  ownership <=512 MiB. The worker also bounds retained derived results plus the
  next bake estimate against a 512 MiB queue budget and drops temporary maps
  after publishing each bundle. Persisted recipe geometry and bundle neighbours also have
  512 MiB limits; recipe metadata <=32 MiB.
- Static opaque/masked target surfaces; positive object scale. Transmissive target
  materials, live modifiers, UDIM, projection cages and high-to-low baking remain
  outside this batch. Emission remains LDR in its separate material map; RGBM
  encoding extends only the new lighting output.

## Verification

The dedicated preparation suite covers generated charts, repeatability, source
UV1 preservation, multiple slots, density/scale, impossible settings, analytic sun
and environment responses, geometric AO and finite-radius falloff, external
occluders, point/area/emissive sources, secondary transport, RGBM overflow refusal,
settings/scene replay, per-object queue errors, history, recipe save/reopen after
bundle deletion, stale rig/neighbour detection and cancellation. Existing material
bake/workflow/layer/preset/polygon suites are also part of focused validation.
Final execution results are recorded in the PR; this document describes contracts
and test coverage, not a claim of discrete-GPU or windowed artist acceptance.

Local validation on 24 October 2026: all six focused suites passed (preparation,
workflow, texture bake/CLI, layered surfaces, material presets and polygon
rendering). CLI replay reproduced all seven PNGs, derived mesh and JSON
provenance byte-for-byte from the exported settings and neighbour snapshots.
All 62 shader contracts and validator self-tests passed.

Integrated upstream Batch 8 review follow-ups: ordinary duplicate/paste remaps both
recipe snapshot owners and baseline hashes; prefab instance updates copy or clear
recipes with geometry; ordered inspection-key hashing preserves source edit
invalidation. Dedicated regressions cover each path, including version-1 recipe
loading and historical prefab hash compatibility.
