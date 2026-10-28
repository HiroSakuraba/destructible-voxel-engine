# PSO-1 — Vulkan driver pipeline caching and cold/warm evidence

## Scope

PSO-1 adds **real VkPipelineCache use** to the DVE 1.0 Vulkan RHI for both
`vkCreateGraphicsPipelines` and `vkCreateComputePipelines`, which previously
passed a null cache handle. This is a driver optimization, not a generic cache
of DVE's IDevice pipeline objects. DVE still creates pipeline objects as
requested and makes no assumptions about driver cache-hit rates.

- Every VulkanDevice creates a shared in-memory VkPipelineCache when supported.
- **Disk persistence is opt-in** with `DVE_PSO_CACHE_FILE=/path/to/cache.bin`
  or `VulkanDevice(std::filesystem::path)`. No file writes by default.
- Driver calls are measured with steady_clock immediately around Vulkan
  graphics and compute PSO creation, with per-device attempts, total
  nanoseconds and maximum single-call nanoseconds.
- Driver cache seed accepted / bytes accepted and flush success / bytes saved
  are exposed through `VulkanDevice::pipeline_cache_stats()`.
- `flush_pipeline_cache()` explicitly writes a snapshot; destructor tries
  again after device-idle. No benchmark claims are made by default.

## Trust and compatibility

Only cache blobs that pass BOTH layers are passed to the Vulkan driver:

1. The versioned DVE envelope has bounded size (16 MiB payload), magic,
   version, payload length, FNV-1a checksum and reserved-field validation.
   It records vendor ID, device ID, **driverVersion**, **apiVersion** and the
   16-byte `pipelineCacheUUID` returned by the selected physical device.
2. The inner Vulkan cache header is checked for minimum size,
   `VK_PIPELINE_CACHE_HEADER_VERSION_ONE`, matching vendor/device and UUID.

Missing, truncated, maliciously large, corrupted, cross-GPU, cross-driver,
changed-API or nonmatching-UUID files are ignored. If a Vulkan ICD rejects
a seed despite a valid header, initialization retries using an empty cache.
If VkPipelineCache entry points are missing or creation fails, DVE continues
with null VkPipelineCache, as before. The cache never changes material output.

Disk snapshots use a unique sibling temporary file and same-directory rename.
Windows uses `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
to replace existing files; Linux/macOS use atomic `std::filesystem::rename`.
The prior valid file is never removed before replacement. On failures,
`diskSaveSucceeded == false` indicates that no new snapshot was published.
**Do not use a network share or an untrusted writable cache directory.**
The cache is not a durable cross-device asset and must never be shipped as
a universal precompiled PSO package.

## Reproducing measurements

On a machine with Vulkan (Lavapipe ICD is acceptable for correctness):

```sh
cmake -S . -B build-pso1 -G Ninja -DDVE_BUILD_TESTS=ON \
  -DDVE_BUILD_RHI=ON -DDVE_BUILD_EDITOR=OFF
cmake --build build-pso1 --target dve_pso_cache_file_tests \
  dve_vulkan_graphics_tests dve_vulkan_compute_tests
ctest --test-dir build-pso1 -R '^dve_pso_cache_file_tests$' -V

export DVE_PSO_CACHE_FILE="$PWD/build-pso1/pso-cache.bin"
export DVE_REQUIRE_VULKAN=1
rm -f "$DVE_PSO_CACHE_FILE"
echo COLD
./build-pso1/dve_vulkan_graphics_tests
./build-pso1/dve_vulkan_compute_tests
echo SEEDED
./build-pso1/dve_vulkan_graphics_tests
./build-pso1/dve_vulkan_compute_tests
```

Each driver test prints a line with `cache_available`,
`pso_seed_accepted`, `create_calls`, `total_ns` and `max_ns`.
The CI suite uses separate cache filenames for graphics and compute:
each category starts with a **genuinely absent DVE seed**, then replays it
in a second clean process. The seeded process must observe `pso_seed_accepted=1` if the driver supports export
and import. The CI workflow `.github/workflows/pso1-vulkan.yml`
performs these jobs with pinned software Vulkan and saves raw logs.

**Important:** Warm *cache seed accepted* does not prove a per-pipeline
cache hit, a speedup, reduced gameplay stutter or a hardware GPU win.
Driver-internal caches can make the 'cold' process warm even when the DVE
file was deleted. Compile times vary due to thread scheduling and driver
work; numbers must be repeated and compared on hardware.

## Remaining work

- PSO-2: enumerate exact shader+layout+renderpass state combinations for
  incoming scenes/materials before publication, with background work and
  frame-time guards.
- PSO-3: capture actual pipeline usage into versioned warmup manifests
  and prewarm across real gameplay scenes; do not reuse Unreal's PSO format.
- Real hardware test matrix: AMD, NVIDIA, Intel, Windows/Linux; compare
  frame p95/p99 and startup time with cold/seeded/driver-warm conditions.
- Cache lifecycle: explicit engine-level path selection and save on clean
  game exit. Shader updates do not need a DVE file-format change, but
  changing shader/layout permutations may leave old driver entries unused.
- Threading: Vulkan external synchronization applies to use of the same
  VkPipelineCache handle if future PSO-2 introduces multithreaded compile;
  currently creation is assumed to occur on the RHI's owner thread.
