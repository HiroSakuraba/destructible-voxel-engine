# Closed groups, group bounds and pivot

Artist worklist ART-035 (groups behave as one object) and ART-036 (group bounds and pivot).

## What counts as a group

A **group** is an object with no geometry of its own (no voxels, polygon mesh, subdivision cage,
3D text or Gabor volume) that has children. That covers what **Edit > Group Selected**, **Scatter**
and **Import Model** make. A parent with its own geometry (a table with a vase on it) is not a group:
clicks reach its children as before.

Groups start **closed**.

## Selecting

| Gesture | Selects |
| --- | --- |
| Click a piece of a closed group | The **outermost** closed group around it |
| Double-click a piece | Opens that group (one level per double-click) and selects the next level down |
| Left-to-right (window) box | A closed group only when every piece with geometry is inside the box |
| Right-to-left (crossing) box | A closed group when any of its pieces is touched |
| Alt-click / click cycling through a stack | One entry per group, not one per piece |
| Scene Hierarchy click | Exactly the row clicked, open or not (the way to reach a piece without opening) |

**Edit > Open Group** opens the selected group (or the closed group around a selected piece).
**Edit > Close Group** (Ctrl+Alt+G) closes the nearest open group around the selection, together
with the groups inside it, and selects it. With nothing selected it closes every open group.
**Edit > Close All Groups** closes them all.

An open group draws a dim box labelled "Name (open)", and its Hierarchy row ends in `[open]`.

## Moving

A selected group draws one teal box around all its pieces, labelled with its name, and a diamond
at its pivot. Frame Selection uses that box.

- **Move**, **Rotate**, nudges, typed position and rotation, and placement snaps move the group
  **with its pieces**, nested groups included, as one undo step. Before this change, the gizmo
  moved only the empty group transform and left unattached pieces behind.
- **Scale** on a group is refused: "Scale works on pieces: double-click the group (or Edit > Open
  Group) and select them". Voxel scale is per object (voxel size or resample), so there is no single
  group scale to apply.
- A **locked piece** blocks moving its group, naming the piece.

## Pivot

- **Edit > Group Selected** puts the new group's pivot at the **bottom centre** of what it groups,
  so it sits on surfaces and turns about its footprint. It used to be the average of the objects'
  origins.
- **Edit > Edit Group Pivot** (checked while on) switches to Move: Move, Rotate and nudges then
  move only the pivot, not the pieces. Esc, Edit Group Pivot again, or selecting something else
  finishes. It needs one selected group, with no socket-attached pieces.
- **Edit > Group Pivot to Bottom Centre / to Centre** set it from the pieces' bounds (one undo step).
- An imported model's pivot stays at the model origin. Reimport places the new parts relative to
  the group's transform, so moving its pivot would shift them. These commands are disabled for an
  import group, with that reason.

## State

- Which groups are open is editor view state, like isolation. It is not saved with the scene and
  is not an undo step.
- It is cleared when a scene is opened or a new scene or project is made. Groups that stop being
  groups (ungrouped, deleted, given geometry) are forgotten.
- Pivot editing ends when Play or Simulate starts.

## Code

- Controller (`src/editor_native.cpp`, "Groups" block):
  - `is_group`, `viewport_selection_target`, `group_bounds`;
  - `open_selected_groups`, `close_groups_around_selection`, `close_all_groups`;
  - `toggle_pivot_editing`, `set_group_pivot`, `prune_groups`.
- `selection_transform_snapshot` adds selected groups' unattached descendants, except while the
  pivot is being edited. `selection_pivot` uses a single selected group's pivot, and
  `selection_bounds` uses group bounds.
- Click, double-click and box selection resolve pieces through `viewport_selection_target`.
- Menu items `edit.open_group`, `edit.close_group`, `edit.close_all_groups`, `edit.edit_pivot`,
  `edit.pivot_bottom_center` and `edit.pivot_center`, with enabled states and reasons.
- Renderer: the group box, the label, the pivot diamond, the open-group box and the `[open]`
  Hierarchy suffix.

## Tests

`tests/test_editor_closed_groups.cpp` (`dve_editor_closed_groups_tests`):

- **Clicks:** a click resolves to the outermost group; a parent with geometry stays open.
- **Open and close:** Open and Close Group; double-click opens one level at a time; the open labels;
  Close All Groups.
- **Boxes:** window versus crossing.
- **Moving:** nudge and the Move gizmo carry nested pieces, with one undo; Scale is refused; a
  locked piece blocks the move.
- **Pivot:**
  - bottom centre and centre;
  - Edit Group Pivot moves only the pivot, Esc finishes, and selecting elsewhere ends it;
  - a new group's pivot is at the bottom centre;
  - an imported model is a closed group whose pivot is fixed.
- **Pruning:** an ungrouped group is forgotten.

## What's not done

- Selecting a piece through the Hierarchy and moving it does not open its group in the viewport.
- There is no "isolate the open group" view that dims everything else.
- Prefab instances are not treated specially: an empty prefab root with children is a group like
  any other.
