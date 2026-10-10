# Scatter Objects

Pseudo-random placement of copies across a surface, for organic scenes: rocks, plants,
debris. This page covers filling a target surface; painting with a brush is the Scatter
toolbar tool (see [editor-scatter-brush-2026-10-10.md](editor-scatter-brush-2026-10-10.md)). Covers the fill half of ART-093 and part of ART-095 (preview before
committing).

## Workflow

1. Select what to scatter: one or more objects in the scene, and/or a prefab in the Assets
   panel. Then click the **surface** last, so it is the active selection. The surface must be
   a voxel object.
2. **Create > Scatter Objects** (or the command palette: "scatter", "foliage", "populate").
   A preview opens; **the scene is not changed while previewing.** Each planned spot is a
   diamond in its source's colour, and a panel shows the surface, the sources, how many copies
   were placed, the spacing, the seed and the keys.
3. Keys while previewing:

   | Key | Action |
   | --- | --- |
   | Enter | Commit (one undo step) |
   | Esc | Cancel; nothing changes |
   | `[` / `]` | 5 fewer / 5 more copies (1 to 1000) |
   | Shift+`[` / Shift+`]` | Spacing 0.25 m smaller / larger |
   | N | New seed (a different layout) |
   | A | Align to surface on/off |

   Commands: `create.scatter`, `scatter.commit`, `scatter.cancel`.
4. Commit adds an empty group, "Scatter (N)", at the top centre of the surface, with every copy
   under it, and selects the group. One Undo removes the group and all copies.

## Placement rules

- **Deterministic.** The same surface, sources, count, spacing and seed always give the same
  layout (a splitmix64 generator, the same on every platform). N changes the seed.
- **Even spread.** Candidate spots are drawn uniformly over the surface's footprint and
  dropped straight down onto it. A spot is kept only if it is at least the spacing away
  (across the ground plane) from every spot already kept, so copies spread out instead of
  clumping (dart-throwing Poisson-disk sampling). Up to 30 candidates are tried per requested
  copy. When fewer fit, the panel says how many and why (too close, or off the surface) and
  suggests lowering the spacing or the count.
- **Only the surface counts as ground.** Other objects on it, including the sources, are
  ignored, so copies never stack on each other. Gaps in the surface get no copies.
- **Sitting on the surface.** Each copy keeps its source's shape and rotation; the bottom
  centre of its footprint lands on the spot.
- **Align to surface** tilts each copy so its up axis follows the ground's slope. Voxel
  terrain is a staircase whose top faces all point straight up, so the slope is measured from
  the surface heights two voxels away on each side, not from the voxel face.
- **Independent copies.** Every copy is an ordinary editable object. Copies from a prefab drop
  the prefab link. A source's children are copied with it.
- Sources are fixed when the preview opens; a selected object that is the surface, contains
  it, or sits inside it is not used as a source.

## Also fixed

- **Hierarchy overflow.** Once a scene had more objects than fit, Scene Hierarchy rows were
  drawn past the panel into the bottom dock. Rows now stop at the panel, a scroll bar
  appears, and the wheel scrolls; scrolled-out rows are not clickable.
- **Frame All changed the active object.** View > Frame All re-added the selection in id
  order, so the highest id became the active object. It now keeps the active object.
- The X11 editor gained a headless `--select ID` option (repeatable; the last is active) for
  screenshots of selection-driven workflows.

## Integration with Slice, Boolean, isolation and Play

- **One preview at a time.** The Scatter, Slice and Boolean previews draw their panels in the
  same place at the top of the viewport. Starting any of them closes the others (the same rule
  Slice and Boolean already followed), so the panels never overlap.
- **Isolation.** With View > Isolate Selection on, the committed group and every copy join the
  isolated set, so new copies are visible and pickable instead of appearing hidden.
- **Play / Simulate.** Scatter is disabled while playing or simulating, and starting Play or
  Simulate closes an open preview.

## Validation

New `dve_editor_scatter_tests`:

- 30 copies on a 10 x 10 m slab at 1 m spacing: all on the top surface, inside the footprint,
  and at least 1 m apart. The same seed reproduces the layout exactly; another seed differs.
- Wide spacing reports "too close"; a slab with a hole gets no copies over the hole and reports
  "off the surface"; a missing target, a target without voxels and no sources are errors.
- Building and executing: one group plus one object per copy, each with the source's voxels,
  no prefab link and its bottom on the surface; one undo removes everything; redo restores it.
- On a tilted slab, Align to Surface turns each copy's up axis to the surface normal and
  copies stay upright without it. On a stepped voxel hill, most normals lean away from uphill.
- Mixed sources (a scene object and a prefab template) build, and prefab copies are unlinked.
- Controller: the disabled reasons, Frame All keeping the active object, the preview leaving
  the scene and its modified flag untouched, `]`, N and A, Esc cancelling, Enter committing
  and selecting the group, one undo, and hierarchy scrolling with 20 more objects.

Screenshots of the X11 editor at 1280x720: the preview on a hill with Rock and Tree sources
(20 of 20 placed), and the committed scene with the hierarchy scroll bar.

## What's not done

- Count, spacing, seed and Align are set only by keys while the preview is open; there is no
  inspector panel for them and they are not saved with the project.
- No per-copy random rotation (yaw) or scale jitter; every copy keeps its source's rotation
  and size.
- One surface object per fill; scattering across several objects at once (or onto copies)
  is not supported.
- Copies are full, independent voxel objects (no instancing), so very large counts cost as
  much as that many hand-placed objects.
- The open preview is not refreshed when the scene changes underneath it; Enter re-plans
  against the current scene before committing.
- The hierarchy scrolls with the wheel but does not yet scroll to a newly selected row.
