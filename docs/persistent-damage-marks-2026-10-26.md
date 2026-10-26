# Damage marks that are saved and follow fragments (26 Oct 2026)

Stacked on `feature/weathering-damage`. The previous branch had the CPU player keep damage marks
(soot, cracks, fresh interior around craters) in a list of its own. The marks were lost on load,
and pieces that broke off left them behind.

The world now owns the marks.

## GameWorld keeps the marks

- **Every carve leaves a mark.** Both `damage_sphere` and the queued `queue_damage_sphere`
  publication add a crater mark at the carve, in the object's own voxel space (metres), with the
  settings' strengths.
  - A carve that destroys the object leaves none.
  - The queued path uses the centre the job carved, not where the object has moved since.
- **Fragments keep their marks.** Pieces that break off share the parent's voxel space and pose,
  so a mark needs no transforming; it only needs a new owner.
  - Each new fragment, from either path, gets a copy of every parent mark whose reach (radius ×
    the largest of soot reach, crack reach and 1.25, plus 5 %) touches the fragment's occupied
    bounds.
  - The parent keeps its own marks, so the soot and cracks continue across the break.
- **Destroyed objects lose theirs.** Destroying an object, retiring debris or releasing it to a
  pool drops its marks.
- **API.**
  - `surface_marks()` lists the marks.
  - `surface_mark_settings()` / `set_surface_mark_settings()` read and change the settings, and
    refuse out-of-range values. Lowering the limit drops the oldest marks.
  - `add_surface_mark(id, worldCenter, radius)` stamps a mark without carving.
  - `clear_surface_marks([id])` clears all marks, or one object's.
- **The player.** It reads `world.surface_marks()` and no longer keeps a list of its own.
  `PlayerApp::surface_marks()` is gone.

## Saving

| where | how |
| --- | --- |
| `GameWorldSaveState` | `surfaceMarks` (oldest first), `surfaceMarkSettings`, `nextSurfaceMarkSeed` |
| Game saves | section `dve.surface_marks`: settings, seed counter, then each mark (owner, centre, radius, strengths, reaches, seed); written only when there are marks, the settings differ from the defaults or the counter has moved. A mark whose owner is not in the save is refused. |
| Restore | the settings and counter are restored, and marks of objects that no longer exist are dropped |
| `state_hash` | covers the settings, counter and marks once there are marks or the settings differ from the defaults, so worlds that never use them keep their hashes |

The seed counter is saved, so craters made after a load crack exactly as they would have
without the save.

## Checks

`dve_weathering_damage_tests`, `test_world_marks`, uses a 10 × 8 × 2 slab and a 6 × 8 × 2 slab
joined by a one-voxel bridge, on a turned wall:

- **First crater.** It leaves one mark at its object-space centre (within 1e-4).
- **Cutting the bridge.** The smaller slab breaks off. The wall then has 2 marks and the piece
  1: the bridge crater's copy, at the same spot. The first crater is too far away to follow.
- **Capture.** It holds the 3 marks and seed counter 3.
- **Hash and restore.** Clearing the marks changes the hash, and restoring brings back the
  marks and the exact hash.
- **Destroying the piece.** It drops its mark.
- **Settings.** Out-of-range settings are refused. Disabled settings add no marks on a carve or
  a stamp. A stamped mark uses the settings' strengths. Clearing one object works.

`test_game_save`:

- **Destruction.** The tower's blast leaves a mark on the tower and a copy on its broken top.
  Both are encoded, decoded and restored, the hashes match, and re-encoding gives the same
  bytes.
- **Migration tests.** The synthetic v0 and v1 saves clear the marks and drop
  `dve.surface_marks`, since those engines had none.
