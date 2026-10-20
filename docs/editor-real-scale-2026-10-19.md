# Real scale (19 Oct 2026)

Artist worklist ART-027. Builds on the drag snap branch.

## What was already there

Since #108–#110, dragging a Scale gizmo handle stretched voxel objects and meshes through their
per-axis scale (`voxelScale`) without touching the voxels, as one undo step; Ctrl-drag resampled
instead, and Alt made a drag uniform.

## What this adds

- **Uniform handle.** The square at the centre of the Scale gizmo is now a handle: it is larger,
  lights up on hover, and dragging it up or right scales all three axes evenly (Alt still works
  on the axis handles).
- **Inspector Scale X / Y / Z.** A Scale row under Voxel size for voxel objects and meshes (not
  3D text or volumes), showing values like `1.5×`. It works like the other numeric fields: type a
  value, `*=2` and similar, three values at once (`1 2 1`), drag the letter to scrub (Shift fine,
  Ctrl snaps to 0.05), right-click to reset to 1, Tab moves on. Every change is one undo step and
  keeps the voxels. A cell must stay between 1 mm and 10 m on every axis.
- **Voxel > Resample Voxels to Scale.** The explicit destructive step: rebuilds the selected
  voxel objects' cells at their current scale, sets Scale back to 1 and keeps the object in place
  and the same size. One undo step restores the old cells and scale.

Ctrl-drag resampling on the gizmo is unchanged; the status line now says when a drag rebuilds the
voxels and when it keeps them.

The Mass line moved down one row and joined the parts count, to make room for Scale.

## Also fixed

Inspector rows partly under the flag toggles (at large UI zoom) are no longer clickable, so their
boxes can never take a click meant for a toggle.

## Tests

`tests/test_editor_real_scale.cpp`: the centre handle (uniform, voxels kept, one undo), the X
handle, typing, relative and triple values, limits, reset, meshes, the drawn row, scrubbing, and
Resample Voxels to Scale (cell count, unchanged world bounds, undo).
