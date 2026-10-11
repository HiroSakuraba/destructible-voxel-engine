# Multi-object inspector feedback and transform safety (ART-084)

The Inspector displayed only the primary object's policy values, even though its checkboxes
changed every selected object. Mixed selections could therefore look fully checked or unchecked,
and their next click depended on which object happened to be primary.

The Inspector now derives a selection summary:

- Visible, Locked, Anchored, Structural and Collision have On / Off / Mixed states. Mixed has a
  dash and a `(Mixed)` label. The first click on Mixed enables the flag for every selected object;
  the next click on All On disables it. A single undo restores each object's previous value.
- Different positions or rotations display `Mixed; primary ...`. Equivalent quaternion signs
  do not count as different rotations (within floating-point precision).
- Multiple selection shows its count and identifies the primary object. The header says
  `Group edits from primary (m / deg)`. Other object-specific fields and details remain primary-only.

## Typed transforms

The existing group transform semantics are preserved. Position entry moves the selection by
its primary object's position delta. Rotation entry rotates the selection together about the
current pivot, setting the primary object's orientation to the entered XYZ Euler angles. These
are not individual absolute assignments to every object. Clicking a transform field explains
this in the status bar; its initial text is the primary object's value.

Starting a Position or Rotation edit captures the selected ids, primary id, transforms and
pivot. Commit is refused, with a route to reopen the field, if selection, primary, a selected
transform or the rotation pivot changed in the meantime. Deleted or locked members refuse the
whole edit. Escape cancels. Enter without typing does not create an undo entry or round the
transform to the three-decimal display value. Nonfinite numbers are rejected by the existing
parser. Successful typed transforms still commit as one undo step.

## Remaining work

ART-084 remains partial. This does not add per-axis mixed-value editing, individual absolute
batch transforms, shared component-property editing or per-property reset. Native object Scale,
full long-value disclosure and toolbar/inspector keyboard routes remain separate gaps. Artist
acceptance has not been performed.

## Validation

Release build with SDL/native hosts disabled. Six focused tests passed: inspector selection,
authoring gaps (including narrow inspector reflow), honest commands, honest follow-ups, chrome
fitting and layout overlap. The new tests cover all five mixed policies independent of primary,
undo/redo, invalid and empty flag requests, equivalent/different rotations, recording-canvas
labels, relative position/rotation with undo, no-op Enter, changed selection/primary/transform/pivot,
locked and deleted members, invalid numeric input and Escape.

The source manifest and `git diff --check` were checked before publication. Native window and
artist workflow acceptance remain pending.
