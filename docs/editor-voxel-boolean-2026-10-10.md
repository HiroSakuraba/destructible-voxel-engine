# Voxel Union, Difference, Intersection (ART-060)

Authored Boolean operations for voxel objects in the native editor. Runtime destruction could
already split objects; there was no way to combine or carve authored voxel objects. This adds a
backend-independent Boolean core and an editor workflow with preview, commit as one undo step,
and cancel.

## Workflow

1. Select two or more voxel objects (click, then Ctrl+click in the viewport or Scene Hierarchy).
   The **active** selection (the last object clicked) is the **target A**. Every other selected
   object is an **operand B** (B1, B2, ... in ascending object-id order).
2. Open **Tools > Voxel Boolean > Boolean Union / Boolean Difference / Boolean Intersection**,
   the viewport context menu, or the Command Center (Ctrl+K: "csg", "subtract", "intersect",
   "merge", ...).
3. A preview opens. **The scene is not modified while previewing.** The viewport shows:
   - A's bounds in blue labelled "A target", operand bounds in orange labelled "B operand"/"Bn";
   - cells the commit would add (green), remove (red, crossed), or repaint (amber), and for
     Union the cells where B overlaps solid A (cyan outline);
   - a panel with the operation and formula (A + B, A - B, A & B), both objects with voxel
     count and voxel size, overlap / result / added / removed counts, anchor outcome, the
     overlap-material and operand-output policies, diagnostics, and the keys.
4. Keys while the preview is open:

   | Key | Action |
   | --- | --- |
   | Enter | Commit (one undo step) |
   | Esc | Cancel; the scene is exactly as before |
   | 1 / 2 / 3 | Union / Difference / Intersection |
   | S | Swap: the first operand becomes A, A becomes an operand |
   | O | Operand policy: Hide, Delete, Keep |
   | M | Overlap material (Union): keep A's / take B's |

   The same actions exist as commands (`voxel.boolean_commit`, `voxel.boolean_cancel`,
   `voxel.boolean_swap`, `voxel.boolean_operands_hide|delete|keep`).
5. Changing the selection, starting Play/Simulate, or deleting a participant closes the
   preview. Edits to a participant made while the preview is open (another tool, undo, the
   live MCP host) recompute it.

## Policies

**Grid.** The result is written into A's voxel grid, in place; A keeps its id, name,
transform, hierarchy, components and voxel size. An operand whose grid is aligned with A's
(same voxel size, rotation a multiple of 90 degrees about A's axes, offset a whole number of A
voxels) is mapped voxel-for-voxel, exactly. Any other operand is **resampled**: an A cell is
inside B when the cell's centre lies in a solid B voxel. Resampling is shown as a warning
because features thinner than one A voxel can disappear or alias.

**Materials.** Difference and Intersection never invent material: surviving voxels keep A's
material. Union fills new cells with the material of the B voxel covering them (earlier
operands win where operands overlap each other). Where A is already solid, A's material is kept
(default) or, with M / `TakeOperand`, repainted with B's.

**Anchors.** A's anchors survive when their voxel is still solid. Union also transfers B's
anchors (mapped into A's grid) when the mapped cell is solid. Difference and Intersection do
not import anchors. The preview shows kept / dropped / transferred counts.

**Operands after commit.** Hide (default) keeps the operands in the scene, invisible, so they
take no part in play, export or picking but can be shown again to redo the operation with
different settings (CRYENGINE/Unreal-style "consume the inputs" without losing them). Delete
removes them (Undo restores them with the same ids). Keep leaves them untouched. With Delete,
the command is unavailable when an operand is locked, has children, or is an ancestor of A.

**Collision.** On commit the target's merged box collision proxy is rebuilt from the
committed voxels and its box count is reported in the status bar; Play/Simulate build their
physics bodies from the same proxy. If the target has Collision turned off, no proxy is
built and the status says so. (The Inspector's "Collision boxes" row uses the unmerged
per-voxel-run proxy, so its number can be higher than the merged count in the status bar.)

## Undo and failure

Commit executes one `CompoundCommand`: `ApplyVoxelBooleanCommand` (sorted voxel change list
plus the anchor set before and after) and the operand policy (`SetObjectFlagsBatchCommand` for
Hide, `RemoveObjectsCommand` for Delete). Undo restores A's voxels, materials and anchors and
the operands (visibility, or the removed objects with their ids, hierarchy and data); Redo
re-applies both. The voxel command checks that every voxel still holds the expected material
and that the anchor set matches before it runs, so a stale command fails without changing
anything. If any part of the commit fails (for example A was locked while previewing), the
compound rolls back, the document is unchanged, and the preview stays open.

The selection is set to A after commit; undo/redo do not change the selection (as with the
other editor commands).

## Diagnostics and disabled reasons

Commands are disabled with a reason that says what to select or change, for example "Only one
object is selected. Ctrl+click a second voxel object...", "'Group' has no voxels; Booleans
combine voxel objects only. Ctrl+click it to deselect it.", "Target 'Wall' is locked. Turn off
Locked in the Inspector first.", or "Stop Play or Simulate (Shift+F5) before running a Boolean."

Before commit the preview explains results that cannot or should not be committed:

| Case | Severity |
| --- | --- |
| Intersection with no overlap, or nothing inside every operand (empty result) | blocks commit |
| Difference with no overlap (no change) | blocks commit |
| Difference that removes every voxel of A (empty result) | blocks commit |
| Union whose operands lie entirely inside A (no change) | blocks commit |
| Intersection where A lies entirely inside the operands (no change) | blocks commit |
| Union with no overlap (result has separate pieces) | warning |
| Operand resampled into A's grid | warning |
| Empty operand (ignored) | warning |
| Estimated work above 2^26 voxel visits (excessive cost) | blocks commit |

## Code

- `include/dve/voxel_boolean.hpp`, `src/voxel_boolean.cpp` (dve_core): `compute_voxel_boolean`
  takes the target and operands as `VoxelBooleanVolume {voxels, world transform, voxel size,
  anchors}` and returns a sorted change list in A's grid, the resulting anchors, statistics,
  optional overlap cells and diagnostics. It does not mutate anything and does not depend on
  the editor. `apply_voxel_boolean_changes` applies or reverts a change list.
- `include/dve/editor_voxel_boolean.hpp`, `src/editor_voxel_boolean.cpp` (dve_editor):
  selection validation with disabled reasons, `ApplyVoxelBooleanCommand`, and
  `EditorVoxelBooleanSession` (preview, recompute, commit, cancel).
- `NativeEditorController`: commands, keys, context menu, preview markers and panel
  (`src/editor_native.cpp`, `src/editor_native_renderer.cpp`).
- Tests: `dve_voxel_boolean_tests` (core) and `dve_editor_voxel_boolean_tests` (editor).

## Not in this change

- Slice (plane cut into two objects) and mesh/polygon Booleans: ART-060 covers voxel
  Union/Difference/Intersection only.
- Producing a new result object instead of modifying A in place.
- Open mesh / self-intersection diagnostics from ART-066 apply to mesh Booleans and have no
  voxel equivalent.
- A 3D translucent ghost of the result; the software viewport shows changed cells as markers.
- Undo and redo do not restore the selection.
