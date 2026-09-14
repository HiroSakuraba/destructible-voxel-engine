# Destructible Voxel Engine v2.34

A destructible voxel game engine with a multi-backend rigid-body physics system,
in-editor audio synthesizer, and sprite/menu tooling.

## Physics backends

Three physics engines behind one solver-neutral `IRigidBodyWorld` contract:

| Backend | Role | Version |
|---|---|---|
| **Jolt** | Primary 3D rigid-body solver (automatic first choice) | 5.6.1 |
| **Box3D** | Optional secondary 3D solver | 0.1.0 |
| **Box2D** | 2D sprite / tile physics | 3.2.0 |

Backend selection is explicit and never silently falls back: `Automatic` prefers
Jolt, then Box3D, then the engine's deterministic reference solver. The engine
also ships its own deformation and fluid solvers alongside the rigid-body backends.

## What's in this repo

- `engine/` — full engine source (C++23, CMake). Includes the Jolt/Box2D
  compatibility fixes and the Box3D 0.1 backend adapter.
- `thirdparty/` — physics library sources at the exact pinned versions
  (Jolt 5.6.1, Box2D 3.2.0, Box3D @ `23861418`).
- `deps/` — prebuilt static libraries (Linux x86-64, Release) with headers and
  CMake package configs, so you can build without compiling the physics SDKs.
- `docs/` — build notes (`PHYSICS_SETUP.md`) and test logs.

The v2.34 complete bundle (this repo as a zip) is attached to the
[v2.34 release](../../releases).

## Quick build (Linux)

```bash
cmake -S engine -B build \
  -DDVE_ENABLE_JOLT=ON -DDVE_ENABLE_BOX2D=ON -DDVE_ENABLE_BOX3D=ON \
  -DCMAKE_PREFIX_PATH=$PWD/deps
cmake --build build -j$(nproc)
ctest --test-dir build
```

To rebuild the physics libraries from source instead of using `deps/`, point
`DVE_JOLT_SOURCE_DIR` / `DVE_BOX2D_SOURCE_DIR` / `DVE_BOX3D_SOURCE_DIR` at
`thirdparty/<lib>`, or use `DVE_FETCH_JOLT=ON` / `DVE_FETCH_BOX3D=ON` to fetch
the pinned upstream commits automatically.

System packages needed for the full build: a C++23 compiler, CMake 3.22+,
`libjpeg-dev`, `libpng-dev`, `libsndfile-dev`, FFmpeg dev libraries.

## Test status

Full suite: **135 / 138 tests pass**, including all Jolt, Box2D and Box3D
physics tests, the menu command-center tests, and the audio/editor synthesizer
tests. The 3 remaining failures are pre-existing and unrelated to the physics
work: one reference-solver assertion bug and two UDP networking tests that
cannot run in sandboxed environments. See `docs/test-logs/`.

## Notes

- Verified on Linux (x86-64). Windows/macOS builds are plausible — the CMake
  files carry MSVC/Apple handling and all three physics SDKs support those
  platforms — but have not been run yet.
- Box3D is an early-stage (0.1.x) engine; it stays opt-in while Jolt remains
  the default production choice.
