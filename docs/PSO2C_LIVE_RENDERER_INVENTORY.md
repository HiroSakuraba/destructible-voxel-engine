# PSO-2C — Exact native pipeline inventory and adoption in DVE renderers

## Background

PSO-1 (#252) gives Vulkan an in-memory and optionally persistent driver pipeline
cache. PSO-2A/B (#253, merged) introduced a bounded scene-generation-aware RHI
pipeline preparation queue, real compiled-handle transfer and an optional gate
for region-streaming publication.

PSO-2C integrates that preparation queue into both active Vulkan-capable
renderer resource families:

1. **Live Environment Renderer** — inventory skybox, physically based material,
   optional clear-glass transport, opaque depth shadow, and optional masked
   depth shadow, derived from the actual shader bytecode supplied to the
   renderer. Descriptors retain their exact shader bytecode, vertex layout,
   attachment formats, bind-group layout handles, depth state and cull mode.
2. **DASHR conservative shell renderer** — inventory diagnostic shell, optional
   PBR shell and optional displaced shadow caster, with exact resource layout
   and render pass state.

These are not fake shader-name placeholders: every staged descriptor is passed
to `IDevice::create_graphics_pipeline` through the PSO-2 queue. The renderers
retrieve native `GraphicsPipelineHandle` objects from the queue **only after
all required descriptions succeed**, and commit the complete resource bundle
before the first frame. The queue never creates pipelines inside frame recording.
`preparedPipelineVariants` exposes the actual number of variants compiled
for the bundle; tests assert the correct optional counts.

On failure, the preparation queue destroys any compiled-but-unclaimed
pipelines **before** tearing down the bind-group layouts they reference. The
previous live renderer is retained until replacement preparation succeeds.
This behavior is important for shader hot reload and partially failed scenes.

## Reproduction

```sh
cmake -S . -B build-pso2c -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDVE_BUILD_RHI=ON -DDVE_BUILD_TESTS=ON -DDVE_BUILD_PLAYER=ON
cmake --build build-pso2c --target \
  dve_live_environment_renderer_tests dve_dashr_shell_tests
ctest --test-dir build-pso2c \
  -R '^(dve_live_environment_renderer_tests|dve_dashr_shell_tests)$' \
  --output-on-failure -V
```

`.github/workflows/pso2c-render-inventory.yml` is the dedicated CI contract.
Additional live Vulkan tests in regular CI exercise the same renderer paths.

## Performance and remaining work

These initialization calls are still **synchronous on the RHI owner thread**,
so this PR is NOT a new background Vulkan compiler or a promise of faster
scene loading. Existing renderer code already created these pipeline families
at initialization; PSO-2C now uses the PSO-2 preparation, completeness checks,
ownership and cancellation machinery rather than bypassing it.

The following work is still required for complete just-in-time streamed
material preparation:

- **PSO-2D:** expose a two-phase renderer resource builder so pipeline
  variants can be staged and pumped across multiple loading-loop iterations
  before publication, including loading-screen progress and cancellation.
- Connect streamable material variant discovery to actual scene-owned assets
  rather than enumerating only the fixed renderer shader families.
- Attach already-prepared handles to the production player host before its
  region readiness callback returns true; do not equate a compiled driver
  cache seed with adoption of a concrete pipeline.
- Measure real-world frame-time p95/p99 and shader/PSO creation times across
  cold-cache, seeded-cache and full prewarming conditions on multiple GPUs.
- PSO-3 gameplay trace manifests should include shader/state hashes rather
  than ephemeral RHI handle indices.

All current renderer outputs, material flags and shadow culling behavior
must remain unchanged. No automatic shader permutation explosion is
introduced by this batch.
