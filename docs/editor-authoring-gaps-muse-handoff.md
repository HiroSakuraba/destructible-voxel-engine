# Editor authoring gaps: implementation and Muse handoff

Base: DVE main `c84b9a0` (rebased after the starter-cube/gizmo merge). Worklist references: ART-005, ART-015,
ART-027, ART-052, ART-053, ART-063, ART-068, ART-113.

## What this change connects

**Voxel Slice:** Tools → Voxel Slice → Slice Voxel Object (also searchable
in Command Center). Select one occupied, unlocked voxel object in Edit mode.
The preview starts at its world bounds centre. X/Y/Z set the world plane axis;
arrows move one voxel width along its normal, Shift moves one tenth. 1 keeps
front, 2 keeps back, 3 keeps both as separate objects. Enter commits, Esc
cancels. API callers can use `configure_voxel_slice(point, normal, output)`
for an arbitrary plane. Front is the nonnegative signed-distance half-space;
cells exactly on the plane belong to front. Blue and orange markers identify
which side each cell belongs to; the yellow line shows the normal.

No scene voxels are modified during preview. Changes to the participant,
selection, or entry into Play close the preview. Commit checks the source
revision, rigid transform, voxel width and anchors again. Materials and
world cell coordinates are retained. One compound command changes the source
and optionally adds a sibling; undo/redo restores both halves and anchors.
The original source remains selected through commit and undo/redo.

This is a **whole-cell voxel cut**, not polygon clipping. It does not assign
new cut-face materials. Both halves must be nonempty before commit. Separate
rejects components, prefab links, attachments, and children until explicit
transfer rules exist. It copies ordinary flags, tags, groups and layer.
The new half drops import source/recipe references. Keeping both in a single
voxel object would leave identical occupancy, so the implemented keep-both
choice creates separate objects. ART-063 remains partial relative to a
full mesh clipping tool with cap/material controls and interactive plane handles.

Synchronous computation is capped at 250,000 occupied cells. Preview drawing
is capped at 20,000 cell markers; counts cover every cell. Larger jobs need a
cancellable worker and revision-checked publication. Geometry/collision
queries and subsequent Play sessions derive their proxies from the committed
voxels. Do not interpret this as a running physics-world update: Slice is
restricted to Edit mode.

**Inspector reflow:** below 320 logical pixels, editable property labels and
values stack on separate lines. Field click rectangles follow those rows.
Subsequent details move down and contribute to scroll extent. Scrolled
partially visible fields stay clickable inside the inspector clip. Wide rows
keep the original layout. Name/detail text is elided within panel padding.
Full long-value disclosure and ordinary cursor editing still need improvement;
ART-005 must remain partial until artist acceptance.

## Work that remains: mesh storage and continuous scale

The current `EditorObject` owns voxel, Text3D and Gabor data, but no editable
polygon mesh. Its `RigidTransform` intentionally stores only position and
rotation. Adding a scale member there and changing only the gizmo would make
rendering, ray queries, attachments, physics, saved scenes and exported players
disagree. These changes therefore require a coordinated implementation.

`docs/examples/editor_authoring_samples.hpp` contains **compiled reference
code**, with `dve_editor_authoring_samples_tests`. It is not registered as a
scene component, native UI tool, renderer feature, or physics feature.

### 1. Store and persist a real polygon authoring object

Add a polygon authoring payload to `EditorObject`, with ownership chosen to
make snapshots independent (an `optional<EditableMesh>` works initially).
Extend `clone_editor_object`, document validation, transactional save/load,
prefab capture and instantiate, clipboard, undo and packaging. Use
`cooked_polygon_from_editable` to generate the renderer/runtime asset, retaining
materials and corner attributes. Round-trip a polygon scene before wiring UI.
Keep save format versioning/backward compatibility explicit; old voxel scenes
must load with identity scale and no polygon payload.

Expose `EditableMesh::edges()` and `edge_vertices(EdgeHandle)` using the existing
half-edge records. A handle returned to UI must include its generation. Do not
use triangle/render indices as component identities. The sample resolves edges
by endpoint pairs because the current public mesh API lacks edge accessors;
replace this with actual edge handles in the production picker.

### 2. Introduce affine transforms across the complete object path

The sample `Affine` implements point/vector transforms, inverse,
inverse-transpose normal conversion, composition, ray conversion and scale
about a world pivot. Use a separate authored scale/affine representation;
do not silently change the meaning of `RigidTransform` for rigid-body physics.
Preserve full affine matrices in hierarchy world caches: a rotated child under
a nonuniformly scaled parent can acquire shear. Either support that shear or
reject the combination with a clear reason. Never discard it silently.

Apply scale consistently to:

| Path | Required change |
| --- | --- |
| Scene document/composition | local authored scale, affine world cache, preserve-world reparent, save/load |
| Viewport rendering | affine vertex positions and inverse-transpose normals, winding policy |
| Bounds and box picking | transformed corners/geometry, not unscaled bounds |
| Rays and voxel traversal | inverse-transform ray without normalising local direction, preserving world distance parameter |
| Snap candidates/pivots | identical affine basis to rendering |
| Runtime publication/export | serialised authored scale, collision and rendering share geometry |
| Physics shapes | transformed geometry and centre of mass/inertia; backend limits on nonuniform scaling stated explicitly |
| Attachments/prefabs | scale inheritance and shear policy with reversible world-preserving reparent |

The existing `ScaleVoxelSizeCommand` and `scale_selection_voxel_size(factor)`
already support a positive arbitrary **uniform voxel resize** without changing
cell count. A continuous uniform gizmo can use that command as an interim step,
with a visible uniform-only mode. This does not provide independent axis scale,
general object scale, or affine hierarchy inheritance.

For that interim gizmo, snapshot `{id, before transform, before voxel size}` at
mouse-down; calculate factor from the original state on every move, clamp the
whole selection to the supported voxel-width range, and preview voxel width
and position about the original pivot. Restore snapshots before submitting one
`ScaleVoxelSizeCommand`. Esc restores both position and voxel width exactly.
Never resample the grid in this path. Keep the existing destructive resample
command separately named and opt-in.

### 3. Connect native component selection

Add explicit Object/Vertex/Edge/Face modes to the controller and visible UI.
Initially require exactly one unlocked polygon object. Convert world rays to
local space using the same affine transform as the renderer. Use a screen-space
radius for vertices/edges and nearest positive ray hit for faces. Resolve depth
and through-selection modes explicitly. For box selection, compare projected
component geometry, supporting the existing set/add/subtract/toggle/intersect
operations. Record object identity plus generation-bearing component handles.
Clear or validate selection on topology changes, scene replacement and undo.

Render selected vertices, edges and faces over the actual polygon surface.
Mode indicators and disabled-command reasons must identify the required domain.
Collision hull editing remains a separate domain/payload with explicit
collision-only behaviour; this sample does not implement hull authoring.

### 4. Connect transforms and topology-safe undo

The sample `MeshGesture` resolves selected faces/edges to a unique vertex set,
keeps before/preview copies, transforms from the initial snapshot, validates
connectivity and rejects collapsed faces, and rejects stale commit. Native
integration must bind that preview to rendering and submit a document command
on release. Use world-to-local conversion for world-space edits; normal/local
frames must use the correct basis. Preserve UV/material attributes.

Its area check is not a complete geometry validator: flipped winding,
self-intersection, nonplanar polygons, and manifold changes need explicit rules.
Direct position writes also do not increment `EditableMesh::attribute_revision`.
Add a geometry revision/set-position API and invalidate cooked mesh, bounds,
collision and render caches on preview publication, commit, undo and redo.

The sample mesh content hash catches position changes but is not a collision-free
identity test or an exhaustive attribute-content test. Production stale checks
should use object identity, geometry/topology/attribute revisions and the exact
snapshots expected by the command. Add a `ReplacePolygonMeshCommand` carrying
before and after payloads with preflight for both execute and undo.

## Tests Muse should run in its full environment

Build these focused targets and run their corresponding CTest names:

- `dve_editor_authoring_gaps_tests`
- `dve_editor_authoring_samples_tests`
- `dve_editor_voxel_boolean_tests`
- `dve_editor_chrome_fit_tests`
- `dve_editor_layout_overlap_tests`
- `dve_editor_snap_scale_tests`

Run the native editor with the actual font renderer at 1280×720, 1536×960 and
200% UI zoom, plus narrow/short windows. Exercise property clicking on both
stacked lines, scroll to the final Text3D/Gabor field, verify toggle/dock clicks,
and try long names and long numeric inputs. Capture before/after screenshots.

For Slice, test rotated/translated objects, negative coordinates, material and
anchor retention, plane-through-cell-centre tie handling, all outputs, selection
changes, deletion, locking, stale preview, Play transition, cancel and undo/redo.
Re-enter Play after a cut and inspect collision on each newly exposed side.

For the subsequent mesh/scale implementation, add save/reload, runtime export,
ray-distance preservation, normals under nonuniform scale, rotated parents,
world/local pivots, shear/reparent, collision fit and inertia, deleted/reused
handles, multi-object atomic undo and cancellation tests. Run existing scene,
spatial, play-session, packaging, attachment and polygon suites.

## Artist acceptance remains separate

Do not mark partial worklist criteria complete from tests alone. Have artists
place and align objects, deep-pick an occluded object, edit a child in a group,
resize without resampling, slice and restore, edit numeric values in a narrow
inspector, and save/reopen. Record observed task completion, wrong-object edits,
missed cancel/undo behaviour, undiscovered controls and time spent recovering.
Keep unresolved criteria visible in the worklist.

## Local verification

Built the editor library and all seven focused test executables in Release
mode against rebased main `c84b9a0`. CTest passed 7/7: the six targets listed
above plus `dve_editor_tests` (including the incoming starter-cube/gizmo
checks). `git diff --check` and the source manifest freshness check passed.
The canvas checks use a measuring test renderer. No interactive native-host,
GPU, running physics backend, or artist acceptance session was performed here.
