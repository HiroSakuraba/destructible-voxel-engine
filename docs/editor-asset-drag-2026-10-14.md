# Drag assets into the viewport

Artist worklist ART-076: drag from the Assets panel to place, with a ghost on the surface, one
undo step, and Esc to cancel.

## Using it

Press on a row in **Assets** and drag into the viewport. Once the pointer has moved 6 px it is a
drag (a short click still just selects the row). While dragging over the viewport:

- A **cyan box** shows where the asset will sit. Its bottom centre is on the surface under the
  cursor, or on the ground plane (y = 0) when nothing is under it.
- A **cross** marks the drop point.
- A **label** at the cursor says what releasing will do ("Release to place boulder.dvox on Hill
  (Esc cancels)"), or why it cannot.

**Release** places the asset. **Esc** cancels, and so does releasing outside the viewport.

| Asset | Release does |
| --- | --- |
| **Prefab** (`.dveprefab`) | Instantiates it with its bottom centre on the drop point (whatever origin it was authored with). Its roots are selected. One undo step. |
| **Voxel asset** (`.dvox`) | Adds a new voxel object, named after the file, with the asset's voxel size, scale and materials. It is selected and remembers the file as its source. One undo step. |
| **Model** (OBJ, glTF, GLB) | Opens the usual **import options**. On Enter, the imported group lands with its bottom centre on the drop point, and the camera stays put. The group moves with its parts, so Reimport keeps the place. Undo removes it, as with Import Model. |
| **Material** (`.dmat`, `.material`, `.dvematerial`, `.dvempc`) | Drop it **on a voxel object**: an amber box shows the target, and release repaints all its voxels, or only the selected voxels when that object has a voxel selection (M). One undo step. |

The material file is matched **by name** to a material in the project's Material Library, ignoring
case, spaces and punctuation, so `brick_red.dmat` paints with "Brick Red". The editor has no loader
that turns a single material file into a palette entry, so a file with no matching library
material is refused: "No material named … in this project's Material Library".

## Refusals

Each refusal says why:

- Other asset kinds (textures, scripts, audio and so on) list what can be dragged.
- FBX/USD models: only OBJ, glTF and GLB import.
- A prefab or `.dvox` that does not load gives the loader's error.
- Material drops are refused:
  - on nothing, or on an object without voxels;
  - on a locked object ("Unlock … first");
  - when the object already uses that material;
  - above 2,000,000 voxels (select a region first).
- During Play or Simulate, or while a long job runs, nothing is placed.

## Code

- `AssetDragState` and `asset_drag()`, `cancel_asset_drag()` in `include/dve/editor_native.hpp`.
- Controller (`src/editor_native.cpp`, "Drag assets" block):
  - `begin_asset_drag` (press on a row), `update_asset_drag` (pointer move: the hit or the ground,
    the ghost and the message);
  - `finish_asset_drag` (release), `place_dragged_asset`;
  - `load_asset_drag_payload`: the prefab, `.dvox` or material is loaded once, when the drag
    starts.
- Model drops set `pendingImportDrop_`. `ImportOptionsState::dropPoint` carries it into the import
  job, which offsets the group and the parts before adding them.
- Renderer: the ghost box, the cross and the cursor label.

## Tests

`tests/test_editor_asset_drag.cpp` (`dve_editor_asset_drag_tests`):

- **Prefab:** its bottom centre is on the floor at the drop point; it is a prefab instance; one
  undo.
- **`.dvox`:** the new object keeps its source, materials and size; it lands on the floor, and on
  top of a wall; one undo each.
- **Ghost and cancelling:**
  - the ghost box and label while dragging;
  - Esc cancels, and so does releasing over the panel;
  - a click is not a drag;
  - a script explains what can be dragged.
- **Material:**
  - it repaints the whole wall, not the floor, with one undo;
  - only the selected voxels change;
  - an unknown name and a locked object are refused.
- **Model:** the import options remember the drop point; the imported model's bottom centre lands
  there, and the group moves with it.

## What's not done

- Dragging files in from the operating system's file manager (OS drag-and-drop) is not
  connected.
- Snapping the drop to the grid, aligning to the surface normal, and rotating with the wheel while
  dragging.
- A voxel-accurate ghost: the ghost is the asset's bounding box.
- Material drops onto polygon meshes, 3D text or Gabor volumes.
- Dropping several assets at once.
