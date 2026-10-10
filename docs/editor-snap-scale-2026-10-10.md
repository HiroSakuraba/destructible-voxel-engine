# Editor snapping switches, world-grid snap and the Scale tool

Artist worklist items ART-023, ART-024 and ART-027, plus the tool half of ART-114. Builds on
the editor legibility change (toolbar table, viewport gestures).

## What changed

- **Separate snap switches and steps (ART-023).** Move, rotate and scale snapping each have
  an on/off switch (View menu, "Snapping" section: Move Snap, Angle Snap, Scale Snap) and a
  step. The status bar always shows all three, for example
  `Move 0.10m  Rotate 15deg  Scale 0.10x`, or `off`. Holding **Ctrl** during a drag inverts
  snapping for that drag. The switches, the scale step and the grid mode are saved in editor
  preferences; older preference files load with snapping on.
- **Relative or world-grid move snapping (ART-024).** "Move Snaps to World Grid" off (the
  default) quantizes the drag, so an object at x = 0.03 moved one step lands at 0.13. On, the
  object's position on the drag axis lands on a multiple of the step (0.10). Grid mode
  applies to world axes; local-axis drags always use relative steps.
- **Scale tool (ART-027).** A tenth toolbar tool (shortcut R, which already selected "Scale"
  but only printed a status message). It shows square handles on the gizmo. Dragging a handle
  scales the selected voxel objects uniformly about the selection pivot, by changing their
  voxel size and moving their origin; the voxels themselves are not resampled. It snaps to
  the scale step (default 0.10), commits one undoable "Scale objects" command, and Esc cancels
  the drag. The existing double/halve voxel commands remain the separate, explicit
  resampling actions. Per-axis scale is not supported: editor objects have a rigid transform
  with no scale, and voxel size is uniform.
- **Tools say what they need (ART-114).** With nothing selected, Move, Rotate and Scale show
  "Select an object first (Q select tool)" in the viewport instead of gestures that cannot
  work. Scale with only non-voxel objects selected says that it works on voxel objects.
- The status message is now elided before the snap summary instead of running under it.

## Validation

- New `dve_editor_snap_scale_tests`: preference round trip and validation; each switch flips
  only its own setting and shows as a checked menu item; move drags in relative, grid, off and
  Ctrl-inverted modes (an object at x = 0.03 stays off-grid by the same amount in relative
  mode and lands on 0.1 steps in grid mode); snapped and free rotation; scale drag preview, Esc
  cancel, 0.1-step factor, unchanged voxel count, scaling about the pivot, one-step undo and
  redo, and Ctrl for a free factor; the "Select an object first" hint.
- `dve_editor_chrome_fit_tests` passes with ten toolbar tools at every window size and zoom.
- Screenshot of the X11 editor at 1280×720 with the Scale tool active: handles, tooltip
  "Scale (R)", gestures and the status-bar snap summary.
