# Voxel collision correctness — focused runtime batch (28 October 2026)

## Objective

Address concrete gaps behind the artist-editor checklist **ART-097** (collision follows voxel cuts)
and **ART-050** (collision preview after edits), without claiming that a headless reference solver
is a Jolt / Box3D or windowed artist acceptance run.

## Changes

1. `GameWorld::replace_voxel_brick` now preserves the **rigid motion field** of a moving
   object when the authoritative voxel edit shifts its center of mass. The old implementation
   copied linear velocity directly even when angular velocity was nonzero. The rebuilt body
   instead uses `v_new = v_old + omega x (COM_new - COM_old)`, via the same
   `inherited_child_center_of_mass_velocity` helper used for fragments and damage.
   Angular velocity and the world-space object-origin pose are retained.
2. `dve_game_world_tests` adds an axis-scaled, rotating dynamic repair fixture:
   the newly empty voxel strip must be empty in both game-raycast and physics-raycast queries,
   the untouched strip must still collide, the old body handle must be retired, the
   object origin must not teleport, and the COM-dependent velocity correction must be
   observably nonzero.
3. `dve_game_world_tests` adds a second-cut-in-the-same-brick regression with
   static and dynamic bodies. Both removed cells must remain non-colliding after the
   second carve, while unaffected voxels remain solid. The raycasts compare game geometry
   with the reference physics backend.

## Validation commands

```sh
cmake -S . -B build/voxel-collision -DDVE_BUILD_TESTS=ON
cmake --build build/voxel-collision --target dve_game_world_tests dve_network_runtime_tests
ctest --test-dir build/voxel-collision --output-on-failure -R '^(dve_game_world_tests|dve_network_runtime_tests)$'
```

Target names may vary if optional network tests are disabled in the configured build;
`dve_game_world_tests` is the primary regression suite.

## Limitations / remaining checklist acceptance

- These tests are **reference-backend contracts**; no native Jolt or Box3D run and no
  physical Play/Simulate editor walkthrough has been completed in this change.
- Immediate damage still uses `fragment_after_damage` and `rebuild_primary_body`; that path
  can drop the prior body before confirming successful replacement. A robust follow-up
  should stage the geometry and all fragment bodies, then publish or roll back as a unit.
  Avoid claiming ART-097 fully complete until backend rejection and all fragment paths are covered.
- ART-098 (collider picking, including hidden/overlapping proxies) and ART-102
  (contact identifiers, collision masks and stale-build diagnostics) are not implemented here.
- ART-050 still needs progress meaningful within a single giant object, and a windowed review
  of voxel Boolean/Slice/undo/redo and Play transitions.

## Manual acceptance matrix

- Static 8x8 sheet: two non-overlapping cuts in the same brick, undo/redo; then raycast,
  player controller and falling test object through each new hole.
- Dynamic rotating box: edit a corner, keep its object origin fixed, inspect velocity/COM
  continuity and collision in both Jolt and Box3D, where enabled.
- Rotated and non-uniformly axis-scaled voxel object: cut/slice/join, verify new collision
  and inspect world-space face alignment.
- Repeated damage spanning a brick boundary, split fragments, debris cap, save/reload
  and network repair; verify no stale solid collider or missing collision body.
- Inject physics-body creation failure during damage, ensure shape/body generation is
  transactional, then recover cleanly without publishing mismatched visuals.

The intended acceptance bar is **geometry and solver collision parity at the same committed
world tick**. Passing reference tests alone is not sufficient to close ART-097 globally.
