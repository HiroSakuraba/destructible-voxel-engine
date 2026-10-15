# Mirror objects

Mirror the selection across X, Y or Z, as a copy or in place. Voxels and meshes really flip;
objects never get a negative scale.

## Using it

| Command | Does |
| --- | --- |
| **Edit > Mirror Copy X / Y / Z** | Adds a mirrored copy of each selected object (children included), named "Name (mirror)", and selects the copies. |
| **Edit > Mirror X / Y / Z (in place)** | Mirrors the selected objects themselves. |

The mirror plane passes through the **current pivot**, and its normal is the chosen world axis.
The status bar says where it went ("across X = 0 (the pivot)"). To mirror a half model about the
origin, use **Edit > Pivot at World Origin** first. The other pivot choices (Active Object,
Bounds Centre, Cursor Hit) work the same way.

Each command is **one undo step**, including children and every geometry change.

## What happens to each kind of object

- **Voxel objects:** cells flip on their own grid (cell *i* becomes *−i−1* along the flipped
  axis). The occupied volume reflects exactly, with materials and anchors.
- **Polygon meshes and subdivision cages:**
  - Positions and normals flip, and every triangle's winding reverses, so faces still point
    outward.
  - Tangents and bounds are recomputed.
  - Persistent element IDs, edges, creases and UV seams/pins are kept. Corner IDs follow the
    reversed corners, and subdivision control faces reverse with them, so the cage still
    subdivides.
- **Empty and group objects:** only their position and turn change.
- **Children:** each child is mirrored in world space and stays attached or parented as before.
  On a copy, the children belong to the copy.
- **Any orientation is exact:** the reflection is written as an ordinary rotation plus a flip of
  the object's own axis closest to the plane normal. An axis-aligned object keeps its rotation.

## Refusals

Each refusal says why:

- 3D text, which would read backwards, and Gabor volumes;
- prefab instances (unpack first, or mirror the prefab);
- locked objects, in place only (copies of locked objects are fine);
- hidden or isolated-out objects;
- the Voxels or Polygons selection domain;
- Play or Simulate, or a long job running.

## Code

- `include/dve/editor_mirror.hpp` / `src/editor_mirror.cpp`:
  - `mirror_pose`: writes the reflection as T'∘F_e;
  - `mirror_voxel_state`: flips cells and anchors;
  - `mirror_polygon_mesh`: flips positions and normals, reverses winding, swaps corner IDs and
    control-face corners, then refreshes the mesh.
- Controller: `mirror_selection(axis, copy)` and the `edit.mirror_*` / `edit.mirror_copy_*`
  menu items in Edit > Mirror.
  - In place: one `CompoundCommand` of a `TransformObjectsCommand` plus a
    `ReplaceVoxelObjectStateCommand` or `ReplacePolygonCommand`, which carries the subdivision
    settings and IDs, per object.
  - Copies are built already mirrored and added with `AddObjectCommand`.

## Tests

`tests/test_editor_mirror.cpp` (`dve_editor_mirror_tests`):

- **Pose maths:** exact for axis-aligned and arbitrarily rotated objects on all three axes; voxel
  cells and anchors flip.
- **Voxel copy (rotated L-shape with a child):** every cell lands at its mirror image; the child
  is copied, mirrored and parented to the copy; anchors kept; the source untouched; one undo.
- **Voxel in place:** object and child mirror, and one undo restores them exactly.
- **Mesh in place:** bounds flip, faces stay outward (signed volume), persistent IDs are kept,
  the mesh validates, and undo restores the IDs.
- **Subdivision cage copy across Z:** still subdivides, faces outward.
- **Refusals:** nothing selected, 3D text, prefab instance, locked in place, Voxels domain.

## What's not done

These come in the mesh-mirror PR:

- live symmetry while editing half a mesh;
- one-shot mirror and weld of half a mesh;
- mirroring selected faces within a mesh.
