# Continuous uniform scaling and split selection history

Historical PR #108 behavior is recorded below. The axis-scale follow-up is documented
in `voxel-axis-scale-editor-runtime.md`; ordinary handles now change local axes and
Alt during dragging keeps uniform scaling available.

Ordinary Scale gizmo drags now resize voxel objects continuously and uniformly,
using the existing world voxel size. Drag any handle or type a factor such as 1.3.
The scene previews its actual geometry, not just a resampled bounds estimate.
The cells, integer grid coordinates, materials and anchors remain unchanged.
Selected origins move about the chosen group pivot and rotations remain unchanged.
Release creates one undo entry; Escape or focus loss restores the starting sizes
and positions. Existing scale snapping and Shift override apply to pointer factors;
typed factors bypass snapping. All selected sizes stay between 1 mm and 10 m.

Ctrl held at the start of the drag selects the existing destructive axis-resampling
path; Alt makes that path uniform. The mode is fixed for the drag. Explicit rescale
commands remain available. Nonuniform X/Y/Z scaling without resampling is still
not implemented: that requires scaled transforms throughout the engine. Native
mesh scaling and component editing are also outside this batch.

No new scene scale field or runtime transform representation is introduced.
Viewport rendering, bounds, picking, scene serialization/export and play-session
physics already consume voxelSizeMeters. Automated checks exercise fractional
size and group pivot previews, unchanged cells/anchors/revisions, cancellation,
Undo/Redo, picking and save/load. Full native-window, physics play-mode and artist
acceptance testing remain pending in Muse's running-program environment.

Separate Islands and Extract Voxels now keep before/after selection snapshots in
their geometry history entry. Undo restores original objects, primary, selection
domain, voxel owner and occupied selected cells. Redo restores the committed
piece selection. Current visibility/isolation continues to filter restoration;
the isolation view is preserved. Repeated isolated Undo/Redo is covered for both
operations, alongside the previous Boolean, Join, Move and Slice workflows.
