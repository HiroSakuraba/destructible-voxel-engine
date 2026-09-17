# DVE v2.35 Runtime and Tooling Foundations

Version 2.35 establishes small, solver-neutral runtime contracts and deterministic reference
implementations. The APIs are in `include/dve/v235_foundations.hpp`; the rigid-body extensions
are in `include/dve/rigid_body_adapter.hpp`.

## Included foundations

| Area | Implemented contract | Deterministic evidence |
|---|---|---|
| Navigation agents | Path following, waypoint advancement, revision replanning, stuck detection, off-mesh state | Agent reaches a target on a two-polygon mesh |
| Dynamic navigation | Dirty tile tracking from voxel bounds, bounded source replacement, transactional rebuild | A dirty rebuild advances the revision and preserves a valid path |
| Packaging | Stable path ordering, manifest, FNV-1a content hashes, editor stripping, unchanged-entry reuse, mount/read integrity checks | Package build, mount, lookup, and payload verification |
| Profiler | Thread-safe CPU scopes, counters, memory categories, per-frame model, JSON | Scope and counter capture |
| Asset dependencies | Validation, rename fix-up, dependency-first closure, deterministic reimport, source fingerprints | Transitive order and rename tests |
| Input | Prioritized contexts, chords, five trigger kinds, conflict detection, rebind persistence | Press and double-tap recognition plus round trip |
| Save games | Versioned sections, migrations, atomic publish, backup rotation, hashes, recovery | Corrupted primary recovers the previous slot |
| Animation | Humanoid maps, CPU retargeting, morph targets, CCD IK | Rig validation, retarget, and morph checks |
| Physics | Point loads, torque, angular impulse, ray/AABB/sphere query-all, ignore filters, stable ordering, material metadata | Reference-world load and query contracts |
| AI | Typed blackboard, behavior tree, perception query, arrive steering | Deterministic sequence and perception order |
| Networking | Snapshot interpolation, bounded rollback input/state history, spatial relevancy | Interpolation, eviction, and relevancy tests |
| Editor/plugins | Undo/redo operations and owner-scoped extension registry | Execute/undo/redo and unload tests |

## Claim boundaries

- `DynamicNavigationWorld` tracks bounded dirty tiles and replaces bounded source geometry,
  but its current transaction rebuilds the compact navigation mesh. Incremental tile baking,
  border stitching, and concurrent publication remain production work.
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

## Focused validation

From the repository root, run:

```sh
./scripts/run_v235_foundation_tests.sh
```

The script compiles the focused suite with C++23, warnings as errors, and the production source
units used by these foundations. Pass `--sanitize` to add AddressSanitizer and
UndefinedBehaviorSanitizer.
