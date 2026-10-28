# DVE collision workflow closeout candidate — ART-097, ART-050, ART-098, ART-102
28 October 2026 · PR #261

## Status and acceptance boundary

These four checklist items have integrated code and regression fixtures on this PR.
**They are not considered fully accepted or verified by hardware/editor testing yet.**
No local C++ build, Jolt or Box3D physical collision run, Windows windowed inspection,
or real user acceptance was performed in this connector-driven implementation session.
GitHub Actions must execute and pass before merging; a queued/skipped run is not proof.

## ART-097 — collision follows voxel edits

* `replace_voxel_brick` preserves rigid-body point velocities when the center of mass
  changes; angular velocity and authored object pose stay continuous.
* The existing immediate `damage_sphere`, queued `ResumableDestruction`, voxel Boolean,
  and editor Play/Simulate paths use the engine's canonical merged collision-box proxies.
* Synchronous `damage_sphere` now stages the body's replacement before removing the
  old one. One-component damage restores its voxels and sends no event if the backend
  rejects the replacement.
* Multi-component synchronous damage stages all fragment bodies, builds the primary
  replacement last, and publishes geometry/fragment objects only after physics accepts
  the change. On rejected staging it destroys transient bodies, restores the old
  voxel geometry, authored pose and object ID allocation, and sends no damage event.
* Existing queued destruction is already structured around staging collision bodies
  before geometry publication. Its separate regression suite still needs execution.
* Added tests for consecutive cuts in the same brick, 64-column voxel/physics
  ray-query coverage, moving axis-scaled repair and velocity continuity, failed
  static/dynamic single-piece replacement, and failed detached/primary creation
  during a split.

**Remaining gate:** compile and run those tests, Jolt/Box3D integration with scene
reopen and moved/rotated objects, save/reload, and collision shape identity after
undo/redo and queued destruction. An exact visual collider overlay alone does not
prove physical contacts.

## ART-050 — rebuild preview and progress

* Editor collision display uses the same exact merged boxes as runtime.
* The explicit rebuild job now provides per-brick progress *within* one large
  object, not only per-object counts.
* Per-brick builder publishes output only on success; cancellation keeps the prior
  completed cache. Worker snapshots are checked against object revision before
  publication.
* Added equivalence/cancel regression tests (same canonical merged proxy).

**Remaining gate:** observe UI responsiveness and cancellation on a very large
single object with real artist data, including the final box-merging phase; test
changes during a worker job, Undo/Redo and mode transitions.

## ART-098 — collision-only selection

* Collision-shading Select clicks physical proxies independently of artist mesh
  occlusion. Same-pixel repeats cycle overlapped collider hits, stable by world
  distance, object ID and box/piece index. Hidden collidable voxel objects remain
  selectable; disabled/isolated objects do not.
* Supports rotated and anisotropically scaled voxel boxes, certified polygon
  convex-prism pieces, authored convex hulls where the optional mesh-authoring
  backend can triangulate them, and polygon bounds fallback (in physics precedence).
* Selected proxy is highlighted distinctly in the viewport and described in the
  status bar. The collision overlay shows convex piece edges and hull edit points.
* Added hidden overlap, scale, isolation, disabled collider, exact convex piece
  and polygon bounds regression tests.

**Remaining gate:** physical windowed selection review, hull authoring without
the Manifold optional backend, crowded overlapping CAD scene usability, and
stable hull/piece selection across edits. A selectable piece index is ephemeral
and must be re-picked after topology edits.

## ART-102 — meaningful collision diagnostics

* During native Jolt/Box3D Play/Simulate, a bounded allocation-free sink records
  up to 512 solver contact records per fixed tick; no Jolt-worker allocations or
  editor UI calls from the callbacks. Capture is read only after the physics step.
  The overlay shows position, normal and physics-body handle pair, with dropped
  event count. The reference integrator explicitly reports no physical contacts.
* Selected object/pair explanations show collision disabled, empty geometry,
  hidden appearance, stale/not-built cache revision, immovable static/static
  contact, parent-child suppression, DebrisNoSelf filtering, and whether a pair
  is eligible to collide.
* The editor scene's `layer` is **not** a backend contact filter mask.
  Diagnostics display this honestly, rather than inventing a per-body layer/mask.

**Remaining gate:** live native sensor/constraint verification and contact-material
display; report why a particular *runtime* pair was suppressed (beyond authored
class predicates), and test callback lifecycle under high-thread-count Jolt.
No contacts in a scene do not imply a backend error; broad-phase separation
and sleeping bodies must be diagnosed separately.

## Relevant test targets

```sh
cmake -S . -B build/collision-closeout -DDVE_BUILD_TESTS=ON
cmake --build build/collision-closeout --target \
  dve_tests dve_game_world_tests dve_network_runtime_tests \
  dve_editor_honest_commands_tests
ctest --test-dir build/collision-closeout --output-on-failure \
  -R '^(dve_tests|dve_game_world_tests|dve_network_runtime_tests|dve_editor_honest_commands_tests)$'
```

Check the configured targets and optional dependencies before using the commands.
Also run the queued destruction, editor Play/Simulate, Jolt, Box3D, editor
collision-hull/convex-set, and savegame suites available in the build.

## Manual acceptance scenes

1. Static and dynamic plates: drill two consecutive holes in one brick,
   Undo/Redo, drag a capsule/ray and drop a body through both.
2. Rotating dynamic object with nonuniform voxel axes: remove an off-center
   strip and confirm physical COM motion continuity and no pose teleport.
3. A bridge broken into multiple pieces: exercise successful and rejected
   solver body creation, zero and full debris caps, persistent debris and
   inherited angular velocity.
4. Large streamed wall: queued destruction across multiple bricks, follower
   collider queries on the next tick, save/reopen, native contact and collision
   cache revision consistency.
5. Editor collision mode: hidden/overlapped voxel boxes; exact CAD parts with
   holes; authored convex hull; polygon bounds fallback; disabled collision.
6. Debug contact scene: check known Jolt/Box3D contacts, the physical handles,
   contact normals, class filtering and the 512-contact diagnostic drop count.

**Definition of done:** no stale collider after committed geometry edit, no
physics body loss on failed edits, exact collision-proxy selection and user-facing
diagnostics verified in a windowed artist/Play session on supported backends.
