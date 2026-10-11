# Selection restoration with Undo and Redo

Native editor Boolean, Join and voxel Move now record before/after selection with
their geometry history entry. Undo restores the original object set and primary;
Redo restores the committed result selection. Voxel Move also restores the
selection domain, owner and selected occupied cells, alongside its geometry and
anchors. Changing selection between those actions does not overwrite the snapshots.

Snapshots are stored in command history entries, so trimming, clearing and
discarding a redo branch also remove their selection metadata. Failed new commands
retain the existing redo branch. Native Undo/Redo consumes the metadata only after
a successful geometry operation. Core command-stack callers can inspect snapshots
but must restore their own view state.

Restoration preserves current isolation and filters out missing, hidden or
isolated-out objects and unoccupied cells. It does not change the viewport or add
selection-only history steps. Commands without snapshots retain current selection
and prune invalid references.

The regression tests exposed a partial compound Undo: a restored Join operand
remained present if a later child failed on a locked target. Compound Undo now
reapplies already-undone children when a child fails. A rollback failure is reported
explicitly; commands must still provide reversible child operations.

Validation covers repeated Boolean cycles under all operand policies, Join,
overlapping voxel Move and anchors, changed selection/domain, isolation, failed
Undo, history limits, failed execute and successful redo branching. Native window
and artist acceptance testing remain pending. ART-068 and ART-112 remain partial:
this batch does not extend selection restoration to Slice, Separate or Extract,
or deliver mesh component editing, continuous object scale or Slice plane handles.
