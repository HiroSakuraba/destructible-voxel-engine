# Editor snap summary, transform-tool hints and Scale Voxel Size

Artist worklist items ART-023 (status bar part), ART-027 (non-resampling resize) and the tool
half of ART-114. Builds on the editor interaction core (#91) and the editor legibility change
(#92).

## Relationship to #91

The first version of this change added its own snap switches, a world-grid move snap, a Ctrl
"snap off" modifier and a toolbar Scale tool that resized objects by changing their voxel size.
#91 landed first with the same features designed differently. That overlap was resolved in #91's
favour, as the user decided, so the following are #91's behaviour, unchanged here:

- The Scale tool (R, Edit > Scale Tool, no toolbar button; nine toolbar tools). It resamples
  voxels per axis, or uniformly with Alt, about the chosen pivot.
- The snap switches and steps: View > Snapping (Move Snap, Angle Snap, Scale Snap, Absolute Grid
  Alignment), the `translateSnapEnabled`, `rotateSnapEnabled`, `scaleSnapEnabled`,
  `scaleSnapStep` and `absoluteGridSnap` preferences, Increase/Decrease Scale Snap (doubles or
  halves the step within 0.01 to 1).
- Shift inverts snapping for the current drag.
- The Move/Rotate/Scale viewport hints, including world/local.

## What this change adds

- **Snap summary in the status bar (ART-023).** The right side of the status bar always shows
  the UI zoom and all three snap settings, for example
  `UI 100%  Move 0.10m  Rotate 15deg  Scale 0.10x`. A switched-off snap shows `off`, and Absolute
  Grid Alignment adds `grid` after the move step. The status message on the left is elided before
  the summary instead of running under it. The summary itself is elided only if it would take
  more than half the bar.
- **Tools say what they need (ART-114).** With nothing selected, Move, Rotate and Scale show
  "Select an object first (Q select tool)" in the viewport instead of gestures that cannot work
  yet. The key comes from the active shortcut profile.
- **Scale Voxel Size (ART-027).** Two commands in Edit > Transform and the command palette:
  *Scale Voxel Size x2* (`transform.scale_voxel_size_up`) and *Scale Voxel Size x0.5*
  (`transform.scale_voxel_size_down`). Neither has a default shortcut. Each one:
  - multiplies the voxel size of every selected voxel object by the factor and moves each
    object's origin so the whole selection scales uniformly about the current pivot (the same
    pivot the gizmo uses). The voxels are not resampled, so the voxel count and the voxel grid
    are unchanged. The Scale tool and Double/Halve Voxel Object Size remain the resampling
    operations.
  - is one undo step, "Scale voxel size" (`ScaleVoxelSizeCommand`).
  - skips non-voxel objects (empties, 3D text, Gabor volumes) and says so in the status bar.
  - is disabled with a reason when nothing is selected, when no voxel objects are selected, or
    when a selected voxel object is locked.
  - keeps every voxel size within 1 mm to 10 m. Near a limit, the factor is reduced for the whole
    selection, so relative sizes and pivot distances are kept, and the status bar says so. At the
    limit, the command does nothing and reports why.

  Fixed x2 and x0.5 presets match the existing Double/Halve Voxel Object Size commands. The
  editor has no free-standing numeric prompt for menu commands (numeric entry exists only inside
  inspector rows and during a gizmo drag), and repeated presets reach any power-of-two size.

## Validation

- `dve_editor_snap_scale_tests`:
  - the status bar summary at 1280x720 and 1536x960 (100, 150 and 200 % UI zoom) and 800x600,
    including a long message elided before it, and the summary following #91's preferences;
  - the "Select an object first" hint, and that Move/Rotate/Scale keep #91's hints (Shift snap,
    world/local) once something is selected;
  - Scale Voxel Size: pivot math for two objects, unchanged voxel counts and rotations, an
    unmoved non-voxel object, one undo step with an exact undo and a redo, x0.5 after x2,
    disabled reasons (no selection, non-voxel only, locked) and clamping at 10 m and 1 mm;
  - no duplicate menu ids or labels, exactly four View > Snapping switches, no
    `view.snap_to_grid`, the palette finding the commands, and no duplicate preference keys.
- `dve_editor_chrome_fit_tests`, `dve_editor_tests` and the X11 compatibility and Boolean suites
  are unchanged and pass.
