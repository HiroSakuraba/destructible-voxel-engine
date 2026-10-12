# Continuous axis scaling: editor and CPU runtime

This follows the data foundation in PR #109. Scale drags now change positive
object-local cell dimensions (`voxelSizeMeters * voxelScale`), without changing
integer occupancy, material IDs, anchors or voxel revisions.

Select Scale, drag X/Y/Z, or type a factor while dragging. Hold Alt **during the
drag** for uniform scaling that preserves the existing aspect ratio. Holding Alt
before pressing still invokes the editor's camera gesture. Shift temporarily
inverts snapping; typed factors bypass snapping. Hold Ctrl when starting the drag
to use the separate voxel resampling workflow. Escape or focus loss restores the
complete preview; release commits one Undo entry. Effective cell dimensions are
limited to 1 mm–10 m by a common factor across the selection.

Scale handles always use the primary object's local frame, including when Move
and Rotate use world axes. Nonuniform scaling about world axes of a rotated object
would require shear, which rigid poses cannot represent. For a group, each member
changes its corresponding local cell axis; origins scale about the group pivot in
the primary object's frame. Members keep their rotations. This is not a general
affine group transform when members have different rotations.

Editor integration includes picking in meters, bounds/framing, rectangle selection,
voxel selection centers and clipboard dimensions, placement corners, voxel move
steps/previews, collision overlays, Slice classification and snapshot invalidation,
Boolean/Join coordinate maps and cache invalidation, print dimensions, scene export
and prefab fingerprints/axis-only template updates (explicit `voxel.scale` overrides
are retained). The software viewport remains a cell splat preview;
nonunit dimensions use projected cell extents rather than square unit splats.

GameWorld and editor simulation build collision boxes, center of mass, mass and
all six inertia entries from the same dimensions. Fragment and queued destruction
paths retain those dimensions. Ray, sphere and conservative capsule queries use
meters. Damage spheres become local ellipsoids; the original integer rasterizer
remains unchanged for unit axes. The CPU player/reference lighting tracer accepts
per-instance cell dimensions and no longer skips mixed base voxel sizes.

Game saves use schema 4 with an integrity-protected `dve.voxel_axes` section. Engine
migration from schema 3 defaults missing dimensions to unit axes; earlier migrations
chain through it. The object section's binary layout stays unchanged. New saves
are refused by older codecs rather than silently dropping their geometry scale.

The streaming `RuntimeSceneWorld`/GPU upload path still has no cell metric in its
instance contract. It explicitly refuses nonunit DVOX assets. This does not block
the GameWorld CPU player path. See `voxel-axis-scale-muse-handoff.md` for the remaining
GPU implementation and full-program validation. Native SDL interaction, production
physics backends and artist acceptance remain unvalidated here.

Focused tests cover rotated/scaled rays and world depth; mass, inertia and center
of mass; capsule sweeps, overlap and depenetration; immediate/queued damage; fragment
dimensions; save/restore/hash and legacy migration; reference and CPU player tracing;
Slice invalidation; aligned Boolean maps; print dimensions; X/Y/Z, uniform, limits,
cancel/focus loss and repeated Undo/Redo. Existing editor, cooker, save, world,
Boolean, Join, selection, Slice, lighting and player tests also run.

Validation result: 18 selected Release suites passed with the CPU player enabled.
The native SDL editor and streaming GPU shaders were not executed.
