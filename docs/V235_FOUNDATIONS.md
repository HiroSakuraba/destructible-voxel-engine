# DVE v2.35 Runtime and Tooling Foundations

Version 2.35 establishes small, solver-neutral runtime contracts and deterministic reference
implementations. The APIs are in `include/dve/v235_foundations.hpp`; the rigid-body extensions
are in `include/dve/rigid_body_adapter.hpp`.

## Included foundations

| Area | Implemented contract | Deterministic evidence |
|---|---|---|
| Navigation agents | Path following, waypoint advancement, revision replanning, stuck detection, off-mesh state | Agent reaches a target on a two-polygon mesh |
| Dynamic navigation | Dirty tile tracking, bounded source classification, unchanged-polygon reuse, transactional publication, and deterministic border stitching | Destruction disconnects one tile; restoration reuses outer polygons and restores a complete path |
| Packaging | Stable path ordering, manifest, FNV-1a content hashes, editor stripping, unchanged-entry reuse, mount/read integrity checks | Package build, mount, lookup, and payload verification |
| Profiler | Thread-safe CPU scopes, counters, memory categories, per-frame model, JSON | Scope and counter capture |
| Asset dependencies | Validation, rename fix-up, dependency-first closure, deterministic reimport, source fingerprints | Transitive order and rename tests |
| Input | Prioritized contexts, weighted composites, analog thresholds, chords, five trigger kinds, overlap conflict detection, versioned rebind persistence | Composite cancellation/actuation, priority consumption, press/double-tap recognition, v2 round trip, and legacy load |
| Save games | Versioned sections, migrations, atomic publish, backup rotation, hashes, recovery | Corrupted primary recovers the previous slot |
| Animation | Humanoid maps, CPU retargeting, morph targets, CCD IK | Rig validation, retarget, and morph checks |
| Physics | Point loads, torque, angular impulse, ray/AABB/sphere query-all, ignore filters, stable ordering, material metadata | Reference-world load and query contracts |
| AI | Typed blackboard, behavior tree, perception query, arrive steering | Deterministic sequence and perception order |
| Networking | Snapshot interpolation, bounded rollback input/state history, spatial relevancy | Interpolation, eviction, and relevancy tests |
| Editor/plugins | Undo/redo operations and owner-scoped extension registry | Execute/undo/redo and unload tests |

## Claim boundaries

- `DynamicNavigationWorld` rebuilds polygon payloads only for dirty tiles, reuses polygons from
  untouched tiles, and restitches shared-edge portals deterministically. Portal stitching and
  validation currently scan the published polygon set; background tile baking, lock-free
  publication, and persistent per-tile caches remain production work.
- `ReferenceRigidBodyWorld` is a deterministic contract implementation, not a production
  contact solver. Native Jolt/Box3D adapters still need the new query and explicit angular-load
  hooks wired to their SDK-specific collectors.
- Networking provides rollback storage, interpolation, and relevancy primitives. A complete
  transport, authoritative replication protocol, serializer, prediction driver, and security
  policy are outside this foundation.
- Retargeting and CCD IK are CPU reference paths. Import-time rig inference, animation
  compression, blend graphs, constraints, and GPU deformation remain separate systems.
- Plugin registration is in-process metadata and callback routing; ABI-stable dynamic library
  loading, dependency resolution, sandboxing, signing, and hot reload remain future work.
- Archive hashes detect accidental corruption; packages are not cryptographically signed or
  encrypted.
- Input composites currently produce scalar actions. Native 2D/3D vector composites, device
  hot-plug routing, glyph selection, and platform input-device adapters remain integration work.

## Focused validation

From the repository root, run:

```sh
./scripts/run_v235_foundation_tests.sh
```

The script compiles the focused suite with C++23, warnings as errors, and the production source
units used by these foundations. Pass `--sanitize` to add AddressSanitizer and
UndefinedBehaviorSanitizer.
