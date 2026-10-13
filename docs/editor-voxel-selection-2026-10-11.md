# Voxel selection

Artist worklist ART-043 (region selection and manipulation). This also unlocks "separate selected
voxels" (ART-061) and grow, shrink and invert (ART-017) for voxels.

## Selection domain

**M** (or Edit > Select Objects / Select Voxels / Toggle Objects / Voxels) switches what the
**Select** and **Move** tools act on: whole **objects**, or the **voxels** of one object. A chip
in the toolbar shows which: "Selecting: VOXELS (1122)  M objects". Switching to Voxels targets
the active voxel object.

1 / 2 were the first choice, but plain digits are Camera Bookmark Recall in the Camera context,
which overlaps the viewport. **M** is rebindable in the shortcut editor.

A voxel selection lives in one object at a time; picking voxels of another object starts over
there. Selected voxels are outlined in amber.

## Gestures (Voxels domain, Select or Move tool)

| Gesture | Selects |
| --- | --- |
| Click a voxel | Its face-connected piece |
| Click empty space | Clears the selection |
| Left-to-right box | Cells whose whole projected volume is contained in the box. |
| Right-to-left box | Cells whose projected volume touches the box, including clipped cells. |
| Voxel > Selection Brush | Toggle explicit brush selection with Select: drag adds, Ctrl-drag removes; `[` / `]` changes radius. Toggle it off to use boxes. |
| Shift + click or box | Adds |
| Ctrl + click or box | Toggles |
| Ctrl+Shift + click or box | Intersects |

Without **View > X-Ray Selection**, boxes and brushes check candidates against first-hit scene
rays, including self-occlusion and other objects. Bounded samples can miss narrow visible slivers;
a ray-budget overflow refuses the selection instead of applying a partial result.
**Voxel > Click Selection: Single Cell / Connected Piece** switches click behavior. Connected
Piece intentionally selects the entire connected volume, including buried cells.

Alt is not used because Alt+drag orbits the camera in the default navigation.

Rotate and Scale have no voxel gizmo. Use **Voxel > Voxel Region Editing** for grid-aligned
quarter turns, integer scaling and directional extrusion; see
[region tool details](editor-voxel-region-tools-2026-10-12.md).

## Operations

| Command | Effect |
| --- | --- |
| **Delete** | Removes the selected voxels. Anchors on them are removed and come back on undo. With no voxels selected, Delete does nothing; it never deletes the object instead. |
| **Voxel > Extract Selected Voxels to New Object** | Moves the selection into a new sibling, "Name (extract)", with the same transform and voxel size, so nothing moves. Like Separate Islands' pieces it keeps materials, anchors, flags, tags, groups, layer and membership, but not the import source, prefab link or other components. The new object is selected in the Objects domain. Refused when every voxel is selected (that is the whole object), and, as with Slice's Separate and Separate Islands, for prefab instances (unpack first) and objects attached to a socket (detach first). |
| **Copy / Cut / Paste** (Ctrl+C / Ctrl+X / Ctrl+V) | Copy keeps the voxels; Cut also deletes them. Paste adds a new object, "Name (copy)", at the same place, selected in the Objects domain so it can be moved with W. Paste uses whichever clipboard, voxels or objects, was filled last. |
| **Move** (W, drag a gizmo axis) | The gizmo sits on the selection, along the object's own grid axes. Dragging previews where the voxels will land as cyan ghosts and shows the whole-voxel offset in the status bar; release applies it, Esc cancels. Moved voxels replace whatever is at the destination; the cells they leave become empty; anchors move with them; the selection follows. |
| **Voxel > Grow / Shrink / Invert Voxel Selection** | Grow adds the touching voxels, Shrink drops the outer layer, Invert selects the other voxels of the object. |
| **Select All** (Ctrl+A, Voxels domain) / **Voxel > Select All Voxels** | Every voxel of the object. |
| **Voxel > Deselect Voxels** | Clears the selection. |
| **Paint, Remove** | While voxels are selected in that object, only selected voxels change, and the rest of the brush is ignored. Add is not limited. |
| **Flood Fill** | Stays inside the selection; starting outside it is refused. |

Every edit is one undo step. The selection drops voxels that stop existing (undo, other tools),
so it never refers to empty cells.

## Limits and when the selection clears

- A selection holds at most 500,000 voxels (`kMaxVoxelSelection`). A click, box, Grow, Invert or
  Select All that would go past it is refused with a message and the current selection is kept;
  piece, grow and invert searches stop as soon as they pass the cap. Use Separate Islands or Slice
  for whole-object work on bigger objects.
- The selection clears when its object is hidden or isolated out of view (#95), when a scene is
  opened or a new scene or project is made (object ids restart per scene), and when Play or
  Simulate starts (Stop restores the document). Voxels of a hidden or isolated-out object cannot be
  selected, so Delete and Move never act on voxels the artist cannot see.
- Locked objects can be selected but every edit says "Unlock ... first"; a Cut that cannot delete
  leaves the clipboard unchanged.
- One preview at a time (#98): Delete, Extract, Paste, Cut and starting a voxel Move close any open
  Slice, Boolean, Join or Scatter preview first, like Flood Fill and Separate Islands.
- In the Voxels domain, Edit > Cut / Copy / Delete are disabled with "No voxels selected" until
  voxels are selected, and Select All Voxels says "Select a voxel object first" when there is none
  (nothing reports success while doing nothing, #101).
- The highlight costs one set lookup per drawn voxel of the selected object; the Move gizmo's
  centre is cached per selection change rather than recomputed every frame; Move ghosts are capped
  at 20,000.

## Code

- `include/dve/editor_voxel_selection.hpp` / `src/editor_voxel_selection.cpp`:
  - `voxel_connected_piece`, `all_occupied_voxels`;
  - grow, shrink and invert;
  - `voxels_in_screen_rect` (front-facing or through), `voxels_in_cube`;
  - delete, move and extract commands, which use `ApplyVoxelBooleanCommand`, so anchors round-trip
    exactly;
  - `VoxelClipboard`.
- Controller:
  - `SelectionDomain`, `set_selection_domain`, `select_voxels`, `voxel_selection()`;
  - the operations, `voxel_move_preview()`, and the gizmo override in the Voxels domain;
  - menu items `select.domain_*` and `voxel.select_*`, `voxel.extract_selection`.
- Renderer: the amber outline, the move ghosts, and the domain chip.

## Tests

`tests/test_editor_voxel_selection.cpp` (`dve_editor_voxel_selection_tests`):

- **Core:** pieces, grow, shrink, invert and the brush cube; delete, move (including into
  occupied cells) and extract, each with anchors and undo; whole-object extract refused.
- **Domain and gestures:** M and the menu check; the hint and toolbar chip; click a piece; Shift
  adds; Ctrl removes; an empty click clears; a box takes front voxels only, and every voxel with
  X-Ray; the Ctrl+Shift brush; grow, Select All, shrink and invert.
- **Edits:**
  - Delete never removes the object.
  - Copy then Paste makes a new object in place.
  - Extract works and undoes.
  - Cut removes the voxels.
- **Move:** the gizmo drag previews and applies a whole-voxel offset, the selection follows, one
  undo; Rotate refuses.
- **Paint and Flood Fill:** limited to the selection; starting outside it is refused.
- **Pruning:** the selection drops removed voxels.

## What's not done

- Selection changes themselves are not undo steps (Ctrl+Z undoes edits, not picks), and undoing a
  Move leaves the selection on the destination cells (pruned to what still exists there) rather
  than restoring the pre-move selection.
- Lasso selection, and selecting by material.
- Rotate or scale a voxel selection.
- Moving into another object, and keyboard nudging.
- Selections across several objects.
- A soft (falloff) selection.
- Paste always makes a new object; pasting voxels into an existing object is not done.
