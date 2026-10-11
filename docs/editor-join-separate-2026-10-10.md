# Join and Separate Islands

Artist worklist ART-061 (separate connected voxel islands) and ART-062 (join voxel objects
without fusing groups). Slice (ART-063) shipped in #97. Both are under **Tools > Voxel > Join and Separate** and in the Command Center
("join", "merge", "weld", "separate", "split", "islands", "loose parts").

## Separate Islands (`voxel.separate_islands`)

Select one or more voxel objects and run Separate Islands. Each object splits into its
face-connected pieces. Voxels that touch only along an edge or at a corner count as separate
pieces, which matches the engine's structural connectivity.

- **Connectivity is 6-neighbour (face) connectivity.** Two voxels belong to the same piece only
  if a chain of shared faces links them.
- **The largest piece stays in the original object.** It keeps the same id, name, children and
  components (so scripts are never duplicated or lost).
- **Every other piece becomes a sibling** (same parent) called "Name (piece 2)", "(piece 3)" and so
  on, largest first. Each piece keeps the original's transform, voxel size, voxel coordinates,
  materials and anchors, so **nothing moves in the world**.
- **What the pieces copy:** flags, tags, groups and layer. They do not copy components (scripts
  and the like stay on the original) or import sources.
- **Selection:** the new pieces are added to the selection; the original stays the active object.
- **Undo:** the whole selection is one undo step, which restores one object with every voxel and
  anchor.
- **Isolation:** with View > Isolate Selection on, a piece joins the isolated set exactly when its
  original is in it (an isolated-out original's pieces stay isolated out).
- **Collision:** pieces carry the original's collision flag; their proxies are built from the
  committed voxels like any other object.
- **Refusals:** a locked object, a non-voxel object, more than 2,000,000 voxels, an object that is
  already one piece, or one that would split into more than 256 pieces (stray dust: clean it up
  first, so one click cannot add thousands of objects). Each is reported with its reason; the
  status line names the first skipped object when only some of the selection was separated.
- **Same rules as Slice's Separate (#97):** prefab instances ("unpack it first") and objects
  attached to a socket ("detach it first") are refused. Unlike Slice, children and components are
  allowed: the original keeps its id, so they stay on it, which is the explicit transfer rule
  Slice was waiting for.

## Join (`voxel.join`)

Select two or more voxel objects. The **active** (last clicked) one is the target: it keeps its
name, grid and place. Join opens a preview; **nothing changes until Enter**.

- **Objects on the target's grid** (same voxel size, rotation a multiple of 90°, whole-voxel
  offset) are copied voxel for voxel, exactly. They show green, labelled "On grid".
- **Objects off the grid** (a different voxel size, or turned or offset off the grid) show orange,
  labelled "Off grid". The panel says why for each one, for example "voxel size 0.5 m vs 0.25 m
  (coarser: blocks are re-gridded)" or "(finer: thin detail may be lost)". **Join waits for a
  choice every time:**
  - **R** resamples them into the target's grid (cell-centre sampling, as Boolean does);
  - **G** groups the objects instead and changes no geometry;
  - **Tab** makes the next selected object the target and asks again. Use it to pick the finest
    grid as the target so nothing is lost.
- **Enter** joins; **Esc** cancels.

On commit, in one undo step:

- the target gains the other objects' voxels, each keeping its material;
- where pieces overlap, the target's material wins;
- anchors carry over;
- children of the joined objects move under the target (they are not deleted);
- the joined objects are removed.

The target keeps its own tags, groups, layer and components.

Join is Tools > Voxel > Boolean Union with join's own rules: operands are always removed, their
children are kept, overlap always keeps the target, and the explicit off-grid choice.

- **Refusals:** fewer than two objects, a non-voxel object, a locked object, a hidden object, an
  object isolated out of view (View > Isolate Selection), or a target inside one of the other
  objects. Because the other objects are deleted, an operand with components other than
  Tags/Layer/Groups membership, or a prefab instance, is refused too (click it last to make it
  the target instead), and so is an operand with a locked child that would have to move. The
  reason names the object.
- **Flags and materials:** the result is one object with the target's flags and layer; the panel
  says so when an operand's collision, structural, anchored, decorative or layer settings differ.
  Every voxel keeps its material id; only overlapping cells take the target's.
- **Collision:** the target's collision proxy is rebuilt at commit (as Boolean does) and the status
  line reports the box count, or that collision is off on the target.
- **Play mode:** Join and Separate Islands are disabled in Play/Simulate; starting Play closes an
  open Join preview.
- **Preview slot:** Join shares the viewport preview slot with Scatter, Slice and Boolean; starting
  any of them closes the others.
- **Selection:** changing the selection closes the Join preview.

## Code

- `include/dve/editor_voxel_join.hpp` / `src/editor_voxel_join.cpp`:
  - `find_voxel_islands`, `separate_islands_problem`, `build_separate_islands_command`;
  - `EditorVoxelJoinSession`, which uses `compute_voxel_boolean` (Union, KeepPrimary).
- **Controller:**
  - Join: `begin_voxel_join`, `commit_voxel_join`, `cancel_voxel_join`, `voxel_join()`;
  - Separate: `separate_voxel_islands`;
  - keys R, G, Tab, Enter and Esc while the Join preview is open.
- **Renderer:** the Join panel and the target, on-grid and off-grid boxes.

## Tests

`tests/test_editor_voxel_join.cpp` (`dve_editor_voxel_join_tests`):

- **Islands:** counting (an edge contact is separate), largest first, deterministic.
- **Separate:** the original keeps the largest piece, its anchor, components and children; pieces
  are siblings in place, named, with tags and no components; materials and anchors carry over; no
  voxel is lost; one undo; one-piece and locked objects are refused; the controller action selects
  every piece.
- **Join on the grid:** exact copy, the operand is removed, its child moves to the target, no
  voxel is lost, one undo.
- **Rules:** prefab instances and attached objects are not separated; more than 256 pieces is
  refused; under isolation, pieces follow their original. Join refuses operands with components,
  locked children of operands, hidden and isolated-out objects; it states flag differences and
  reports the collision rebuild.
- **Join off the grid:** both mismatches are named, commit waits, R, Tab asks again, the resampled
  result, one undo restores, G groups instead, a selection change closes the preview.
- **Panel and preview slot:** the panel is drawn, and Slice closes Join.

## What's not done

- Separate works on whole connected pieces. Picking which piece to pull out (a "Separate selected
  voxels" mode) needs a voxel selection tool, which the editor does not have yet.
- Join's resample is cell-centre point sampling, like Boolean. There is no supersampling or
  coverage threshold.
- Joined objects' components are not merged into the target; Join refuses operands that have
  any (other than Tags/Layer/Groups membership) instead.
- Undo of a Join restores the operands but leaves only the target selected (undo does not record
  selection anywhere in the editor yet).
- Separate Islands is synchronous; the 2,000,000-voxel and 256-piece caps keep it bounded, but a
  cancellable worker would be needed for bigger objects.
- Children attached to a joined object are re-attached to the target at the same world place
  (their local offset is recomputed); the socket name is kept as is and is not checked against
  the target.
- Join and Separate have no default keyboard shortcut (menu and Command Center only).
