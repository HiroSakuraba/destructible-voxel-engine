# Interactive voxel Slice

Slice now owns ordinary viewport clicks while its preview is open. The object's
Move/Rotate/Scale gizmos are hidden so those clicks cannot change geometry or
selection behind the plane controls. Camera navigation remains available between
plane drags.

- Drag the yellow shaft or its square handles to move along the plane normal.
  When viewed along the normal, drag the collapsed handle vertically instead.
- Drag a coloured ring to rotate the plane about a world axis. The axis parallel
  to the normal is omitted because that rotation would not change the plane.
- Snap defaults to on: translation uses the target's voxel size, rotation uses
  the editor's angle snap preference. Shift gives fine movement; Ctrl temporarily
  enables snapping when the panel toggle is off.
- Release keeps the preview without creating a history entry. Escape during a
  drag restores its starting plane. Escape again closes the preview. Losing
  window focus restores an unfinished drag.
- Click Point or Normal to enter three world-space numbers. Normals are
  normalized. Invalid values remain editable and do not change the plane.
  Numeric input is exact and does not use drag snapping. P/N open those fields;
  S toggles snap. Existing X/Y/Z, arrow, 1/2/3 and Enter shortcuts remain.
- The panel exposes axis presets, Keep Front, Keep Back, Separate, Apply and
  Cancel. Blue and orange cell markers preview the two sides. A blocked operation
  shows its reason. Separate selects both halves and retains the original as primary.

Applying Slice records geometry and selection in one history entry, using the
selection-history foundation from PR #106. Undo/Redo restore object selection,
primary, selection domain, voxel owner and occupied selected cells. Current
isolation and visibility still filter restoration. Cancel adds no geometry or
history. Changing or locking the target closes an active drag safely.

Interactive configuration reuses captured world-space cell centres and updates
their side classification, avoiding sparse-state rebuilds and sorting on every
pointer event. The existing 250,000 occupied-cell preview limit remains.

Automated validation exercises rendered panel controls, numeric validation,
perspective and orthographic translation, end-on translation, angle snapping,
drag cancellation, focus loss, stale targets, click ownership, and repeated
Undo/Redo under all three outputs. Native-window interaction and artist acceptance
must still be checked in the running program. This remains a whole-cell Slice:
it does not cut polygon fragments, add cut-face materials or edit mesh components.
