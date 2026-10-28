# PSO-2A — Bounded preparation and fail-closed scene publication

PSO-1 (#252) implemented opt-in persisted `VkPipelineCache`. That can reduce
driver creation cost, but cannot guarantee that a previously unseen pipeline is
already compiled. PSO-2A adds `PipelinePreparationQueue` to stage **real**
`GraphicsPipelineDesc` and `ComputePipelineDesc` and take their compiled
native RHI handles only after the entire required scene generation is ready.

## Contract

- Device and queue have the same owner thread. Stage from the loading/
  preparation phase; call `pump(maxPipelines, softBudget)` **outside**
  frame recording or scene publication. No worker threads call Vulkan here.
- Queue owns complete descriptor copies including shader bytecode, layouts,
  render-target format, vertex attributes, culling and other pipeline states.
  Do not stage guessed variants: use the exact descriptor later needed.
- Requests have a nonzero scene epoch and unique stable key **within that
  epoch**. Duplicate requests fail closed; 512 requests is the bounded cap.
- Each pump executes at most `maxPipelines` driver calls; the microsecond
  budget is **soft** because Vulkan creation cannot be preempted after it
  starts. A zero-work pump does nothing.
- `ready_to_publish(epoch)` requires one or more requirements and no
  pending or failed pipeline. A failed driver call blocks publication;
  `failure_reason(epoch,key)` explains it.
- `take_graphics` and `take_compute` move actual pipeline handles into the
  runtime resource owner. They perform **zero** pipeline compilation calls.
  A second take returns null. Caller must destroy successfully taken handles
  after GPU completion using the ordinary RHI lifetime rules.
- `discard_epoch` destroys unclaimed completed handles, cancels pending jobs,
  and forgets failures, but never destroys already-published handles. A new
  scene epoch can use the same keys without borrowing old resources.
- **Important**: layout handles in staged descriptions must remain alive
  until compilation, and the device must outlive its preparation queue.
  No queue ownership escapes across a device reset.

Example (schematic; call from a loading phase, **not** inside a frame):

```cpp
dve::rhi::PipelinePreparationQueue queue(device);
queue.stage_graphics(epoch,"level.material.opaque",opaqueDesc);
queue.stage_graphics(epoch,"level.material.masked",maskedDesc);

while (!queue.ready_to_publish(epoch)) {
    auto progress=queue.pump(2,std::chrono::microseconds(2000));
    if(progress.failed) { queue.discard_epoch(epoch); return false; }
    // Yield to the application main loop; never busy-wait in a render tick.
}
auto material=queue.take_graphics(epoch,"level.material.opaque");
auto masked=queue.take_graphics(epoch,"level.material.masked");
// Both handles are already compiled and can be inserted into the scene's
// resource bundle before publishing a new authoritative visible scene.
queue.discard_epoch(epoch);
```

## Validation in this PR

- New `dve_pso2_preparation_tests` on Null RHI: caps, duplicate keys,
  zero-budget, partial pump, complete scene readiness, failed driver creation,
  stale epochs, cancellation, published handle ownership and no double-take.
- Vulkan graphics and compute device tests stage and transfer a **real**
  RHI pipeline before first command recording, using the PSO-1 cache-backed
  Vulkan backend.
- Dedicated `.github/workflows/pso2-pipelines.yml` demands Lavapipe rather
  than silently succeeding when no GPU is available.

## What PSO-2A does **not** do yet

This is the generic, explicit preparation and resource-ownership layer.
Existing Live Environment and DASHR renderer initialization already compiles
their fixed pipeline families outside draw recording. This PR does **not**
replace those renderers' pipeline creation, add a background Vulkan compiler,
or automatically scan every material from a streamed scene.
A production host must still stage exactly the needed descriptors, call
`pump` during loading, and gate scene publication on `ready_to_publish`.
That host integration plus material/scene variant inventory belongs to
**PSO-2B**, after this core is verified. In particular, PSO-2A alone makes
no claim of lower p99 frame time.

PSO-3 then records real variant usage for replay; it must not use unstable
handle indices or raw C++ struct memory as persistent variant keys.
