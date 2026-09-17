# Destructible Voxel Engine v2.35

A C++23 game-engine codebase centered on destructible voxels, hybrid voxel/polygon scenes,
multi-backend physics, simulation, animation, audio, rendering, networking, and editor tools.

Version 2.35 adds deterministic runtime and tooling foundations for navigation agents,
packaging, profiling, asset dependencies, input, save games, animation, physics queries,
artificial intelligence, networking, editor operations, and plugins.

## Quick validation

The focused suite has no third-party runtime dependency beyond a C++23 compiler:

```sh
./scripts/run_v235_foundation_tests.sh
./scripts/run_v235_foundation_tests.sh --sanitize
```

For a CMake build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake 3.24 or newer is required. Optional native backends and desktop integrations are
controlled by the `DVE_ENABLE_*` and `DVE_BUILD_*` options in `CMakeLists.txt`.

## v2.35 foundations

| Area | Included |
|---|---|
| Navigation | Agent path following, waypoint advancement, replanning, stuck detection, off-mesh state, and voxel-edit dirty-region tracking |
| Packaging | Deterministic `.dvepak` archives, manifests, hashes, editor-only stripping, incremental reuse, mounting, and integrity checks |
| Tooling | CPU profiler scopes, timelines, counters, memory categories, asset dependency graph, source monitoring, and reimport ordering |
| Gameplay | Prioritized input contexts, composite bindings, gestures, remapping, versioned atomic saves, migrations, and recovery |
| Animation | Humanoid mapping, CPU retargeting, morph targets, and CCD inverse kinematics |
| Physics | Point forces, torque, angular impulses, deterministic ray/AABB/sphere query-all contracts, filters, ordering, and material metadata |
| AI and networking | Blackboard, behavior tree, perception, steering, interpolation, rollback history, and replication relevancy |
| Editor and plugins | Undoable operations and owner-scoped registration for panels, commands, importers, components, inspectors, and scripting bindings |

The rigid-body abstraction supports Jolt as the primary production 3D route, Box3D as an
optional experimental 3D route, Box2D for 2D physics, and the deterministic reference world
for contract tests. Native backends are opt-in and are not fetched unless explicitly requested.

## Packaging

Build a deterministic package from selected project-relative files:

```sh
dve_pack <project-root> <output.dvepak> <file> [file ...]
```

Or scan the project root while applying the default editor-only exclusions:

```sh
dve_pack <project-root> <output.dvepak> --all
```

## Repository layout

- `include/dve/` — public engine interfaces
- `src/` — engine implementations
- `apps/` — command-line tools, demos, and editor entry points
- `tests/` — deterministic and integration tests
- `assets/`, `examples/`, `shaders/` — source-controlled runtime content
- `third_party/` — bundled notices, ABI shims, and reference material
- `scripts/`, `tools/` — validation and release utilities
- `docs/V235_FOUNDATIONS.md` — implemented boundaries and remaining production work

## Release integrity and scope

`SOURCE_MANIFEST.sha256` records the source-release file hashes. `release-manifest.json`
describes the source-only profile and the historical payload intentionally excluded from this
repository.

The v2.35 APIs are tested foundations, not a claim that every production integration is
complete. In particular, stitched partial navigation rebuilding, native Jolt/Box3D query
collectors, the complete network transport/replication driver, and ABI-stable dynamic plugin
loading remain follow-up work. See [the v2.35 foundation notes](docs/V235_FOUNDATIONS.md) and
[the changelog](CHANGELOG.md) for details.
