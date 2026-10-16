# Named selection sets (16 Oct 2026)

Artist worklist ART-021, and the "save useful queries" part of ART-019. Builds on the select by
property branch.

## Using them

Edit > Selection Sets:

- **Save Selection as Set** (Ctrl+Shift+G): saves the selected objects, or the selected
  vertices, edges or faces of the mesh being edited. A name field opens at the top of the
  viewport with a default name ("Objects 1", "Box faces 1"); type a name and press Enter, or Esc
  to keep the default. Saving and naming are one undo step.
- **Save Filter as Smart Set**: saves the hierarchy filter (a name or a query such as
  `type:mesh tag:rock`). A smart set is re-evaluated each time it is recalled.
- **Recall Selection Set...** (Ctrl+Alt+G), **Add Selection to Set...**, **Rename Selection
  Set...**, **Delete Selection Set...**: each opens a list of the sets, for example

      Rocks  (objects, 12)  · 1 missing
      Lid  (faces, 2 on Box)
      Smart  (query, type:voxels -tag:rock = 7)

Recalling a component set selects its mesh and switches to the right component mode. Adding
needs the same kind of selection (objects to an object set, faces of the same mesh to a face
set). Every change (save, add, rename, delete) is one undo step, and the sets are saved with the
scene.

## Missing and retired members

- **Missing**: the object was deleted, or the element no longer exists and nothing records what
  replaced it. Undo of the delete brings it back.
- **Retired**: a topology edit removed the element without successors (for example a dissolved
  edge).

Both are counted in the list and in the status line on recall; the set itself keeps them, so
Undo restores the full set.

## Following topology edits

Component sets store persistent modeling IDs, not vertex or triangle indices. When an edit on a
mesh that a component set uses replaces elements (loop cut, inset, extrude, bevel, dissolve,
cleanup), the document records which IDs replaced which ("selection lineage"). On recall a member
that no longer exists is followed through that record, so **both halves of a split face stay in
the set**. The record only grows: after Undo the original ID exists again and resolves to itself;
after Redo the pieces resolve again. Meshes without component sets record nothing.

Lineage is recorded where the native editor commits topology edits (tool previews, bevel,
dissolve, cleanup). Document-level fallback paths that rebuild identities without lineage leave
replaced members as "missing" rather than guessing.

## Saving

Scenes with selection sets are written as scene format version 13: a `selection_sets` section and
a `selection_lineage` section after the modeling sections. Scenes without sets keep their older
version, so nothing changes for them. Version 13 needs this build or later to open.

## Code

- `EditorDocument::selection_sets()`, `selection_lineage()`, `record_selection_lineage()`
- `include/dve/editor_selection_sets.hpp`: resolving a set, describing it, and the undoable
  `ReplaceSelectionSetsCommand`
- `src/editor_native_selection_sets.cpp`: the Edit menu commands, list and name field

## Tests

`tests/test_editor_selection_sets.cpp`: object sets (save + name as one undo step, recall, list
labels, add, missing after delete and back after Undo, rename, delete and Undo), smart sets,
face sets through an inset with Undo and Redo, a real loop cut splitting a saved cage face into
two members, retired versus missing, lineage only for meshes with sets, and save/load of sets and
lineage.
