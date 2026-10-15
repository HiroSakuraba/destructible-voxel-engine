# Voxel brush preview and sphere brush

Artist worklist ART-044: show what a voxel brush will touch before clicking, and offer a sphere
brush.

## Preview

With the **Add**, **Remove** or **Paint** tool, the cells that a click would change are outlined
under the cursor:

| Tool | Outlined cells | Colour |
| --- | --- | --- |
| Add | Empty cells the brush would fill (centred one cell out from the face under the cursor) | Green |
| Remove | Occupied cells it would empty | Red |
| Paint | Occupied cells of another material | The active material's colour |

A label at the cursor gives the count, shape and radius, for example "+68  sphere r3",
"-81  cube r2" or "paint 9  sphere r2  (selection only)". A cube brush also draws its reach as a
grey box.

The preview uses the same rules as the click:

- Add only fills empty cells.
- Paint skips empty cells and cells that already have the material.
- With a voxel selection in that object (#102), Remove and Paint only touch selected cells, and
  the preview says "(selection only)".
- On a locked object the label says "Locked: unlock … to edit", and nothing is outlined.

The cells are recomputed only when the cursor cell, brush, tool, material, object or selection
changes, not every frame. At most 20,000 cells are outlined; the count stays exact.

## Sphere brush

**Voxel > Brush Shape: Cube / Sphere** (radio, checked in the menu) and **Toggle Brush Shape**
choose the shape. Cube stays the default, so nothing changes until you pick Sphere. `[` / `]` still
change the radius (0–32), and the status bar names the shape.

The shape applies everywhere the voxel brush does:

- Add, Remove and Paint clicks and strokes;
- selection-limited Remove and Paint;
- the voxel **Selection Brush** (Voxels domain).

A sphere of radius r takes the cells within r of the centre cell (`sphere_brush_voxels`, already
used by the engine's brush command).

## Code

- Controller:
  - `voxelBrushShape_`, `voxel_brush_shape()` and `voxel_brush_radius()`;
  - `voxel_brush_preview()`, which returns `VoxelBrushPreview` (the cells, count, operation, shape,
    and the selection-only and locked flags) and is cached by `BrushPreviewKey`;
  - `in_voxel_brush`;
  - actions `voxel.brush_shape_cube`, `voxel.brush_shape_sphere` and `voxel.brush_shape_toggle`.
- `apply_voxel_tool` passes the shape to `make_brush_command`. The selection-limited path and
  `voxel_brush_select_at` filter by it.
- Renderer: the cell outlines, the cube reach box and the cursor label. Tool hints mention the
  shape and colours.

## Tests

`tests/test_editor_brush_preview.cpp` (`dve_editor_brush_preview_tests`):

- **Add:** the preview of a radius-1 cube on a top face is 18 cells; the label and outlines are
  drawn; the click adds exactly those cells.
- **Sphere:** the menu check and toggle; a radius-3 sphere removes 76 cells, fewer than the cube;
  the click removes exactly the preview, with one undo.
- **Paint:** with a selection, only the 9 selected cells in reach change, as labelled; afterwards
  the same spot has nothing left to change; a locked object previews nothing.
- **Selection Brush:** with a sphere it takes no cube corners.

## What's not done

- Box and Beam tools have no preview (they are fixed-size stamps).
- A falloff or strength brush, and other shapes (cylinder, disc).
- Brush shape and radius are not saved between sessions.
