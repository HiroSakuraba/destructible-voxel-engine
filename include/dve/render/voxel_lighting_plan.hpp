#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dve/render_environment.hpp"

namespace dve::render {

inline constexpr std::uint32_t kMaximumVoxelLightingSamples = 16U;
inline constexpr std::uint32_t kVoxelLightingThreadsPerGroup = 64U;

enum class VoxelLightingPass : std::uint8_t {
    GenerateShadowRays,
    TraceShadowRays,
    ResolveShadows,
    GenerateGlobalIlluminationRays,
    TraceGlobalIlluminationRays,
    ResolveGlobalIllumination,
    ShadePrimary,
};

struct VoxelLightingDispatch {
    VoxelLightingPass pass{VoxelLightingPass::ShadePrimary};
    std::uint64_t elements{};
    std::uint32_t groupsX{};
};

// Backend-independent dispatch and storage contract for the shader passes. Ray buffers use a
// fixed 16-entry stride per pixel, matching the HLSL generators/resolvers, while activeRayCount
// reports the actual quality cost for telemetry.
struct VoxelLightingFramePlan {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t pixelCount{};
    std::uint64_t shadowRayCapacity{};
    std::uint64_t activeShadowRayCount{};
    std::uint64_t globalIlluminationRayCapacity{};
    std::uint64_t activeGlobalIlluminationRayCount{};
    std::vector<VoxelLightingDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] VoxelLightingFramePlan make_voxel_lighting_frame_plan(
    std::uint32_t width, std::uint32_t height, const RenderEnvironment& environment);

} // namespace dve::render
