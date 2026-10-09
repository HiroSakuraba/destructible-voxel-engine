# Queued destruction: box limit, debris cap and tick budget

Fixes to `GameWorld::queue_damage_sphere` (the resumable path from the Clockwork
integration), plus one fix shared with `damage_sphere`.

## What changed

- **Box limit after merging.** The queued path built one collision box set per brick and
  rejected any piece with more than 256 boxes before merging them. A piece spanning more
  than 256 bricks (for example a 128×128×8 static wall) was therefore always rejected, after
  doing all the preparation work. Boxes are now merged with `merge_adjacent_voxel_boxes`,
  as the synchronous path does, before the 256-box admission limit is checked.
- **Static collision validated.** A rebuilt static primary body is checked with
  `validate_static_rigid_body_desc` and marked `RigidBodyCollisionClass::Full`, as in the
  synchronous path. An invalid description rejects the transaction.
- **Debris cap.** At the cap, a queued commit was rejected outright, while `damage_sphere`
  retires the oldest debris. Queued commits now retire the oldest debris too. Retirement
  cannot be undone, so the commit first checks that enough other debris exists, creates all
  bodies, and only then retires.
- **Never retire the object being split.** `retire_oldest_debris` takes the id of the
  object being split and skips it. Hitting a debris piece while the debris cap was full could
  make `damage_sphere` retire (destroy) that same piece while `fragment_after_damage` was
  still using it.
- **Shared tick budget.** Only the head request ran each tick, so twelve small hits took at
  least twelve ticks. Units left after a request commits, fails or goes stale now go to the
  next request in the same tick, in FIFO order (at most 64 requests per tick).
- **Untouched bricks skip the damage raster.** The raster phase built a scratch object and ran
  the damage raster for every brick of the object. Bricks the sphere cannot reach (with one
  voxel of margin) now copy their voxels directly. Unit counts are unchanged, so saved
  cursors resume the same way.

## Not changed

- The 32-piece admission limit still rejects the whole request, where `damage_sphere` splits
  the 32 largest pieces.
- Each queued hit still processes the whole object voxel by voxel (connectivity and the
  rebuilt primary piece), so the delay grows with object size, not hit size: about 17 ticks
  for one hit on a 64×64×8 wall at the default 4096 units per tick, and about 65 ticks on a
  128×128×8 wall. Removing that needs incremental connectivity, not a budget change.

## Validation

New tests in `dve_game_world_tests`:

- `test_queued_damage_on_a_large_wall_commits`: one queued hit on a 128×128×8 wall commits and
  opens the collision body (previously rejected).
- `test_queued_damage_shares_the_tick_budget`: twelve queued hits on small objects commit in
  one tick (previously twelve).
- `test_queued_damage_retires_old_debris`: at a debris cap of 1, a queued split retires the
  older fragment (previously rejected).
- `test_splitting_debris_at_the_cap_keeps_the_piece_being_split`: a debris piece hit at a
  full cap survives the split, and the piece that has no slot is dropped.
