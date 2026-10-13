# Scene conversion, voxel previews and visible selection

Follow-up to PR #117. Source review only: no builds, automated tests or CI were run.
The commit includes [skip ci].

## Conversion in the scene

Select one visible, unlocked source object. Polygon > Convert Selected Mesh to Voxels or
Voxel > Convert Selected Voxels to Polygon opens a preview. Apply adds a new sibling, retains
the source, selects the output and records one geometry/selection Undo transition. Cancel
changes no source geometry. Conversion uses the current owned mesh or voxel cells, not its
import file. Outputs retain cached world placement, grid scale, flags, tags, groups and layer.
They do not copy import recipes, prefab links, sockets, collision hull components, scripts or
children. A converted prefab source creates an independent root; a socket-attached source
creates an unattached result at its current world pose. The source retains its anchors.

| Direction | Controls and output |
| --- | --- |
| Polygon to voxels | Source-local cell size 0.005..10 metres; Solid, Shell or Surface. Existing object scale is retained, so a scale of 2 doubles the world cell width. Base-color/alpha textures and vertex colors are sampled; UV1 base color is routed through the voxelizer's UV0 sampler per material. Color variants are reduced to available palette slots. Detailed shaders, normal/detail maps, material layers and smoothing are not baked. |
| Voxels to polygon | Exposed solid/air faces become two triangles each, with outward flat normals, material groups and UV0/UV1 unit-square coordinates per face. These UVs repeat and overlap; they are not an atlas. No internal occupied/occupied faces are emitted. The owned result is editable with polygon tools. |

Object-owned voxel material overrides use IDs reserved outside the existing editor paint
palette and occupied scene IDs. The preview never changes the shared material library.
Colors and material definitions are written into DVOX; scene format 8 records which IDs are
explicit overrides. Scenes without overrides continue using format 6/7. Old scene formats
still load. New palette scenes require this reader. Main viewport, camera preview, clipboard,
Extract, scene cloning and Play density use or retain these overrides. Painting with existing
editor palette IDs retains their meaning. Definitions have no copied layer-stack references.

Conversion is bounded: 2,000,000 dense working cells, 16,000,000 summed triangle bounding-cell
visits, 500,000 input triangles, 1,000,000 input vertices, 64 MiB decoded input images, 250,000
input voxel cells / exposed output faces, and the available 8-bit palette slots. Block-surface
output is not guaranteed manifold for edge/corner-touching cells; this command does not
perform print-export manifold repair. Open polygon surfaces can leak in Solid mode; inspect
the voxelizer warning and use Shell/Surface when appropriate.

## Region settings and preview

All existing signed extrusion, rotation-axis and scale-factor menu choices now open the
region panel. Click a numeric field or type a number; Tab moves between the amount and custom
pivot fields. X/Y/Z chooses the object-grid axis. Signed extrusion accepts +/-1..32 layers,
rotation accepts +/-90 degrees, and uniform integer scaling accepts factors 2..8.
Rotation pivots use Bounds, Cell 0 or Custom cell-centre coordinates. Consecutive rotations
keep their previous pivot; explicitly choosing Bounds recomputes it from the current region.

Update Preview computes geometry on a snapshot through the existing cancellable background
job service. It creates no undo command or source edits. Changed fields invalidate the previous
result. Apply rechecks scene and region state; stale results recompute and require inspection
before another Apply. Escape or Cancel discards the preview. Cancelling a background preview
discards its result even if the bounded computation finishes later.

Region previews support source objects up to 2,000,000 occupied cells and the existing 500,000
selected-output-cell cap. The viewport shows at most 20,000 result cells or triangles, while
the operation and collision checks use the complete bounded result. Unselected source cells
remain visible. Occupied unselected destinations still refuse the complete operation.
Apply/Undo/Redo reuse PR #117's expected-state geometry, anchor and selection transitions.

## Selection

Single Cell / Connected Piece are explicit click modes. Connected Piece includes the whole
connected volume by design. Box gestures retain left-to-right window, right-to-left crossing,
Shift add, Ctrl toggle and Ctrl+Shift intersect. Selection Brush remains explicit.

With X-Ray off, projected box candidates and brush cells are accepted only when a sample ray's
first scene hit is that exact object and voxel. This handles other-object and concave
self-occlusion and respects visibility/isolation and near/far planes. Box samples come from
the clipped cell silhouette inside the box. Brush samples use exposed face interiors.
This is sampled visibility: narrow visible slivers can be missed. No full raster visibility
buffer is claimed. Work is capped at 100,000 rays per box/brush dab; exhaustion refuses that
selection rather than applying a partial subset. X-Ray bypasses occlusion.

## Manual handoff (not executed)

- Convert an edited mixed-material polygon, including UV1 base-color texture, in all fill modes.
  Change size, inspect warnings, cancel once, then Apply. Confirm both source and output remain,
  world placement/scaling match, colors survive save/reopen, and Undo/Redo restores selection.
- Convert a mixed-material voxel object with negative grid coordinates and nonuniform scale.
  Inspect outward normals, absent internal faces, material groups and repeating UV0/UV1.
  Confirm source anchors/children stay on the source and the mesh remains editable.
- Exercise extrusion +/-1..32, rotations +/-90 around each axis and all pivot modes, and scale
  factors 2..8. Cancel each preview once. Check collisions, source locks/hiding, four consecutive
  turns of an even-width region, stale snapshots and geometry/anchor/selection Undo/Redo.
- Check number replacement, Backspace, Tab, X/Y/Z, Update, Apply, Enter and Escape in a narrow
  viewport. Cancel an active preview job; confirm its late result never changes the scene.
- Compare Single Cell and Connected Piece clicks. Box/brush a target behind another object,
  in a concavity and across near-plane clipping, in perspective and orthographic views.
  Repeat with X-Ray. Check budget refusal leaves the old selection intact.
- Exercise conversion size/raster/image/palette limits; check rejection creates no scene objects
  or materials. Source object IDs may be reserved while a conversion preview is open.
