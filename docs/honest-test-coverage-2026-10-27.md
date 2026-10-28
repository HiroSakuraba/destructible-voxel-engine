# Honest test coverage and stale-result guards (27 Oct 2026)

These are two lessons taken from hdUE's verification rules:
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

**At configure time** (`cmake/DveTestCoverage.cmake`, evaluated by a call deferred to the end of the
top-level `CMakeLists.txt`, so it sees every target and test whatever the include order, region and
packaging tests included):

- **Coverage report.** Every optional test group has a short id and is listed as built or SKIPPED,
  with the reason and the option that controls it:

  | id | group | controlled by |
  | --- | --- | --- |
  | `Lua` | Lua scripting | `DVE_ENABLE_LUA` |
  | `Vulkan` | Vulkan GPU tests (they still skip at run time on a machine with no device) | the Vulkan backend (`dve_rhi_vulkan`) |
  | `DXC` | DXC shader conformance | a DXC with SPIR-V codegen |
  | `Jolt` | Jolt | `DVE_ENABLE_JOLT` |
  | `Editor` | Editor | `DVE_BUILD_EDITOR` |
  | `Player` | Player runtime | `DVE_BUILD_PLAYER` |
  | `AssetPipeline` | Asset pipeline | `DVE_BUILD_ASSET_PIPELINE_TESTS` |
  | `MeshAuthoring` | Manifold mesh authoring | `DVE_ENABLE_MESH_AUTHORING` / `DVE_ENABLE_PRINT_EXPORT` |
  | `AudioSynth` | Audio synthesis | `DVE_BUILD_AUDIO_SYNTH` |
  | `Box2D`, `Box3D` | Box2D, Box3D | `DVE_ENABLE_BOX2D`, `DVE_ENABLE_BOX3D` |
  | `Brepkernel` | Exact CAD geometry through brepkernel | `brepkernel` runnable at configure time (`DVE_BREPKERNEL`, `brepkernel` on PATH, or `python3 -m brepkernel`) |

- **Where it shows up.** The report is written to `<build>/dve_test_coverage.txt`. Configuration
  prints it as a **warning** when any group is missing.
- **In every ctest run.** The test `dve_test_coverage` prints the report and is **Skipped**
  (`SKIP_REGULAR_EXPRESSION`) whenever a group is missing, so every ctest summary shows the gap.
  ctest still exits 0 when tests are only Skipped: that is what ctest does with `SKIP_RETURN_CODE`
  and a matched `SKIP_REGULAR_EXPRESSION`. No workflow greps for "100% tests passed".
- **Required groups, per job.** No job has every group (hosted runners have no GPU and no DXC, the
  plain Release job has no Lua or Jolt), so *each job names the groups it must have*:
  `-DDVE_REQUIRE_TEST_GROUPS=Lua,Jolt` (a CMake cache variable; or the environment variable of the
  same name when the cache variable is empty; separators `,` `;` or spaces). Configuration **fails**
  when a named group is SKIPPED, when a name is unknown (typo guard), or when groups are named with
  `DVE_BUILD_TESTS=OFF`. Without it a runner that loses a package, or an edited preset, only turns a
  group into a "Skipped" line that nobody reads.
- **A job that can run everything.** `-DDVE_REQUIRE_COMPLETE_TESTS=ON` requires *every* group.

**What each job requires** (the "required groups" are checked at configure time; the run-time column is the
environment variable `DVE_REQUIRE_COMPLETE_TESTS=1` for the ctest step, which turns any run-time skip into a failure):

| workflow / job | required groups | run-time strict | why not more |
| --- | --- | --- | --- |
| `ci.yml` ctest `linux-gcc-release`, `linux-gcc-asan` | Vulkan, Editor, Player, AssetPipeline, MeshAuthoring, AudioSynth | no | no Lua / Jolt in these presets; no DXC in the image; the hosted runner has no Vulkan device, so the Vulkan GPU tests exit 77 |
| `ci.yml` ctest `linux-gcc-lua-release` | the above + Lua | no | same |
| `ci.yml` ctest `linux-gcc-player-release` | the above + Lua, Jolt | no | same; no Box2D / Box3D preset |
| `ci.yml` windows-msvc | Lua, Jolt, Vulkan, Editor, Player, AssetPipeline, MeshAuthoring, AudioSynth | no | no Vulkan device, no DXC |
| `v235-foundations.yml` native physics (Jolt, Box3D) | Jolt / Box3D | yes | the selected adapter test may not skip |
| `rdv-bbv.yml` | Vulkan | yes (with `DVE_REQUIRE_VULKAN`) | has a software ICD (lavapipe) |
| `region-index.yml` ctest steps | none (RHI off) | yes | the selected contract tests may not skip |
| `texture-t1.yml` native formats | Vulkan, Player | no | `dve_vulkan_texture_t1_format_tests` exits 77 without a device |
| `exact-tube.yml` | Editor, Brepkernel | yes (with `DVE_REQUIRE_BREPKERNEL`) | the only job with a pinned brepkernel; the exact tube and exact CAD tests may not skip there. Everywhere else their real-geometry parts exit 77 |

The per-leg list of `ci.yml` is the `DVE_EXPECTED_TEST_GROUPS` job env (a map keyed by preset); a preset
missing from the map fails the Configure step instead of silently dropping the requirement.
To add a job: look at `dve_test_coverage.txt` in a green run, list the `built` groups the job really
has, and pass them with `-DDVE_REQUIRE_TEST_GROUPS=...`.

Checked locally: configuring with a group disabled and required fails (`-DDVE_ENABLE_LUA=OFF
-DDVE_REQUIRE_TEST_GROUPS=Lua`, `-DDVE_ENABLE_VULKAN_BUFFER_BACKEND=OFF ...=Vulkan`, an unknown id,
`DVE_BUILD_TESTS=OFF` with a group named), the environment-variable form works, and every job's list configures
cleanly.

**At run time** (`tests/dve_test_skip.hpp`):

- **Every test can be Skipped.** Every test that has no `SKIP_RETURN_CODE` of its own gets 77 (the
  two live-editor MCP tests already return 77 on platforms without Unix sockets and keep theirs).
  Tests of subdirectories are covered too (CMake 3.28+), vendored dependencies are left alone.
- **The helper.** A test that cannot run some of its checks calls
  `dve::testing::note_skip(reason)`, which prints `SKIPPED: reason`. It then returns
  `dve::testing::exit_code(failed)`: 1 if anything failed, else 77 if anything was skipped,
  else 0. CTest therefore shows such a run as **Skipped**, never Passed, and a failure still
  outranks a skip.
- **For CI.** With the environment variable `DVE_REQUIRE_COMPLETE_TESTS=1` (any value except
  empty, `0`, `false`, `off`, `no`), any skip is a failure.
- **Tests converted from "print and return 0":**
  - the GPU parts of the AO, atmosphere and radiance-cascade probe tests;
  - the whole Vulkan device, camera-output, compute, graphics, DASHR, environment-lighting,
    live-renderer, material-table, sprite and T1-format tests, the DXC clear-glass and surface
    tests, and the Vulkan part of the depth-only and RDV BBV tests;
  - the Jolt part of `test_collision_convex_set`;
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

- **Configuration.** Each CI preset's list of required groups was configured locally (and the
  hosted runs print the same report): Linux Release and ASan have every group but Lua, DXC, Jolt and
  Box2D/3D; the Lua leg adds Lua; the player leg and Windows add Jolt; no job has DXC or Box2D/3D.
- **An expected group that is missing fails.** Verified by configuring with the group disabled and
  required: `-DDVE_ENABLE_LUA=OFF -DDVE_REQUIRE_TEST_GROUPS=Lua`,
  `-DDVE_ENABLE_VULKAN_BUFFER_BACKEND=OFF -DDVE_REQUIRE_TEST_GROUPS="Lua;Vulkan"`,
  `DVE_REQUIRE_TEST_GROUPS="Lua Jolt"` in the environment (Jolt off), an unknown id, and
  `DVE_BUILD_TESTS=OFF` with a group named all fail configuration with the report;
  `-DDVE_REQUIRE_COMPLETE_TESTS=ON` fails while DXC/Jolt/Box2D/Box3D are off.
- **ctest still exits 0 when something is Skipped** (checked locally and by the green hosted runs); with
  `DVE_REQUIRE_COMPLETE_TESTS=1` in the environment the self-skipping tests fail instead.
- **What is Skipped on the hosted runners, and why.** Linux: the Vulkan GPU tests (no device), the DXC
  tests, `dve_test_coverage`; in the plain Release leg also `test_game_save` / `test_game_scene_loader` /
  `test_collision_convex_set` (no Lua / Jolt). Windows: the same GPU tests and the two live-editor MCP
  tests (no Unix sockets).
