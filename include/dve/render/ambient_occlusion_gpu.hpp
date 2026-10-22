#pragma once

// The GPU ambient-occlusion passes, run through the RHI on one voxel object:
//   1. primary_brickmap_trace  traces the camera rays            -> primary hits
//   2. generate_ao_rays        n cosine-weighted rays per hit     -> AO rays
//   3. primary_brickmap_trace  traces the AO rays                 -> AO hits
//   4. resolve_ao              distance-weighted visibility       -> one AO value per pixel
// recorded in one command list with buffer barriers between the passes, then read back.
//
// Rays are in the object's voxel-index space (the brickmap's). The shaders are the HLSL sources
// compiled to SPIR-V with register classes shifted apart for Vulkan:
//   register(bN) -> binding N, register(tN) -> binding 16 + N, register(uN) -> binding 32 + N,
// all in set 0 (dxc -spirv -fvk-b-shift 0 0 -fvk-t-shift 16 0 -fvk-u-shift 32 0; see
// shaders/compiled/ao/README.md). reference_gpu_ambient_occlusion is the CPU mirror of all four
// passes (raycast_packed_brickmap_hdda for the traces, reference_ambient_occlusion for the AO).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/packed_brickmap.hpp"
#include "dve/render_environment.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct GpuAmbientOcclusionShaders {
    std::span<const std::byte> trace;      // primary_brickmap_trace
    std::span<const std::byte> generate;   // generate_ao_rays
    std::span<const std::byte> resolve;    // resolve_ao
};

struct GpuAmbientOcclusionResult {
    std::vector<GpuTraceResult> primary;   // per pixel
    std::vector<float> ambientOcclusion;   // per pixel; 1 where the primary ray missed
    std::uint32_t samplesPerHit{};
};

// Binding numbers (register + class shift) the host and the compiled shaders agree on.
inline constexpr std::uint32_t kGpuConstantBinding = 0U;    // register(b0) ... b2 -> 0 ... 2
inline constexpr std::uint32_t kGpuShaderResourceShift = 16U;   // register(tN) -> 16 + N
inline constexpr std::uint32_t kGpuUnorderedAccessShift = 32U;  // register(uN) -> 32 + N

// Camera rays through pixel centres, in the brickmap's voxel space (row-major, top row first).
[[nodiscard]] std::vector<GpuTraceRay> gpu_primary_rays(Float3 position, Float3 target, Float3 up,
                                                        float verticalFieldOfViewRadians, std::uint32_t width,
                                                        std::uint32_t height, float maximumDistance);

bool execute_gpu_ambient_occlusion(rhi::IDevice& device, const PackedBrickmapScene& scene,
                                   std::span<const GpuTraceRay> primaryRays, const RenderEnvironment& environment,
                                   float metersPerVoxel, std::uint32_t frameSeed, const GpuAmbientOcclusionShaders& shaders,
                                   GpuAmbientOcclusionResult& result, std::string* error = nullptr);

[[nodiscard]] GpuAmbientOcclusionResult reference_gpu_ambient_occlusion(const PackedBrickmapScene& scene,
                                                                        std::span<const GpuTraceRay> primaryRays,
                                                                        const RenderEnvironment& environment,
                                                                        float metersPerVoxel, std::uint32_t frameSeed);

} // namespace dve::render
