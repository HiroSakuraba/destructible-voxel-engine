# Honest test coverage and stale-result guards (27 Oct 2026)

Stacked on `feature/texturing-gaps`. These are two lessons taken from hdUE's verification rules:
"a successful exit code alone is insufficient", and "frame IDs and generations prevent stale
results from being presented as current".

## 1. Skipped tests are reported, not counted as passes

A build with optional test groups switched off still printed "100% tests passed". This session
was caught out three times:

- **Lua.** It was off in the working build, so the Lua tests of four branches never ran, and two
  of their fixtures were broken.
- **DXC.** The GPU shader conformance test needs DXC, which isn't installed here, so it was
  never built.
- **The player.** It was off too, so the player changes in the damage-marks and texturing
  branches were never compiled until this branch turned it on.

**At configure time** (`cmake/DveTestCoverage.cmake`, included after every test is registered):

- **Coverage report.** Every optional test group is listed as built or SKIPPED, with the reason
  and the option that controls it:

  | group | controlled by |
  | --- | --- |
  | Lua scripting | `DVE_ENABLE_LUA` |
  | Vulkan GPU | the Vulkan backend |
  | DXC shader conformance | a DXC with SPIR-V codegen |
  | Jolt | `DVE_ENABLE_JOLT` |
  | Editor | `DVE_BUILD_EDITOR` |
  | Player runtime | `DVE_BUILD_PLAYER` |
  | Asset pipeline | `DVE_BUILD_ASSET_PIPELINE_TESTS` |
  | Manifold mesh authoring | `DVE_ENABLE_MESH_AUTHORING` / `DVE_ENABLE_PRINT_EXPORT` |
  | Audio synthesis | `DVE_BUILD_AUDIO_SYNTH` |
  | Box2D, Box3D | `DVE_ENABLE_BOX2D`, `DVE_ENABLE_BOX3D` |

- **Where it shows up.** The report is written to `<build>/dve_test_coverage.txt`. Configuration
  prints it as a **warning** when any group is missing.
- **In every ctest run.** The test `dve_test_coverage` prints the report and is **Skipped**
  (`SKIP_REGULAR_EXPRESSION`) whenever a group is missing, so every ctest summary shows the gap.
- **For CI.** `-DDVE_REQUIRE_COMPLETE_TESTS=ON` makes configuration fail instead.

**At run time** (`tests/dve_test_skip.hpp`):

- **Every test can be Skipped.** Every test gets `SKIP_RETURN_CODE 77`.
- **The helper.** A test that cannot run some of its checks calls
  `dve::testing::note_skip(reason)`, which prints `SKIPPED: reason`. It then returns
  `dve::testing::exit_code(failed)`: 1 if anything failed, else 77 if anything was skipped,
  else 0. CTest therefore shows such a run as **Skipped**, never Passed, and a failure still
  outranks a skip.
- **For CI.** With the environment variable `DVE_REQUIRE_COMPLETE_TESTS=1`, any skip is a
  failure.
- **Tests converted from "print and return 0":**
  - the GPU parts of the AO, atmosphere and radiance-cascade probe tests;
  - the whole Vulkan device, camera-output, compute, graphics, DASHR, environment-lighting,
    live-renderer and material-table tests;
  - the Jolt parts of `test_game_save` and `test_game_scene_loader`;
  - the Lua script-state part of `test_game_save`, which used to be silently compiled out.

  The existing per-test switches (`DVE_REQUIRE_VULKAN`, `DVE_REQUIRE_AO_GPU`, …) still turn a
  skip into a failure for that one test.

## 2. Stale results can't be shown as current: already in place

An audit of DVE's asynchronous producers found every one already guarded, and most already
tested:

| producer | guard |
| --- | --- |
| Material previews and thumbnails (`MaterialPreviewWorker`) | request generation: a result is published only if no newer request arrived; `test_editor_material_presets` checks that a stale job cannot replace the latest |
| Editor long jobs (import, flood fill, bake, …) | scene fingerprint taken at start: a result is not applied if the scene changed while it ran, and the user is told to run it again |
| Asset scans | the result is dropped when the project root changed |
| Exact-CAD descriptions | keyed by the source's SHA-256 (content-addressed, so never stale) |
| Runtime scene staging | residency generation and asset path checked before publication |
| Autosave | writes a snapshot cloned on the UI thread; never published into the live document |

There was nothing to fix, so this branch only records the audit.

## Checks

- **Configuration.** Configuring the working build (Lua and the player on; Vulkan, DXC, Jolt
  and Box2D/3D off) lists 5 groups as SKIPPED with their reasons and warns.
- **ctest.** It now runs 387 tests: 0 fail, and 6 are reported as Skipped instead of Passed.
  - `dve_test_coverage` (the 5 missing groups);
  - the GPU parts of the AO, atmosphere and radiance-cascade probe tests (no Vulkan);
  - the Jolt parts of `test_game_save` and `test_game_scene_loader`.
- **Turning the player on.** This added the 15 player tests. They compiled and passed, which is
  the first time the player changes in the damage-marks and texturing branches were built.
