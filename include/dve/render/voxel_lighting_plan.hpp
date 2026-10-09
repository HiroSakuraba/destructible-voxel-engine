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
    BuildRadianceCascadeProbes,
    TraceRadianceCascadeIntervals,
    TraceRadianceCascadeSun,
    MergeRadianceCascade,
    GatherRadianceCascades,
    ShadePrimary,
};

inline constexpr std::uint32_t kNoRadianceCascadeLevel = UINT32_MAX;

// Conservative initial GPU budget from docs/RADIANCE_CASCADES.md §7.2: 4-pixel probes and
// a 4x4 full-sphere-equivalent direction grid, stored as eight hemisphere directions.
// The CPU reference defaults are deliberately higher quality (2 pixels, 8x8).
struct GpuRadianceCascadeSettings {
    // The live GPU path still uses VoxelOneBounce. An executor must explicitly opt into
    // this pass contract after it implements the shaders, buffers, and barriers.
    bool enabled{false};
    std::uint32_t baseProbeSpacingPixels{4};
    std::uint32_t baseDirectionResolution{4};
    float baseIntervalLength{1.0F}; // voxel units
    float intervalGrowth{4.0F};
    std::uint32_t maximumCascades{0}; // 0 = distance/screen-limited, at most 10
};

struct GpuRadianceCascadeLevel {
    std::uint32_t probeSpacingPixels{};
    std::uint32_t probesX{}, probesY{};
    std::uint32_t directionResolution{}; // full-sphere equivalent; half is stored
    float intervalStart{}, intervalEnd{}; // voxel units
    std::uint64_t probes{}, directionTexels{}, radianceBytes{}, probeBytes{};
};

struct VoxelLightingDispatch {
    VoxelLightingPass pass{VoxelLightingPass::ShadePrimary};
    std::uint64_t elements{};
    std::uint32_t groupsX{};
    std::uint32_t cascadeLevel{kNoRadianceCascadeLevel};
};

// Backend-independent dispatch and storage contract for the shader passes. Ray buffers use a
// fixed 16-entry stride per pixel, matching the HLSL generators/resolvers, while activeRayCount
// reports the actual quality cost for telemetry.
struct VoxelLightingFramePlan {
    std::uint32_t width{};
    std::uint32_t height{};
    GlobalIlluminationMode globalIlluminationMode{GlobalIlluminationMode::Off};
    std::uint64_t pixelCount{};
    std::uint64_t shadowRayCapacity{};
    std::uint64_t activeShadowRayCount{};
    std::uint64_t globalIlluminationRayCapacity{};
    std::uint64_t activeGlobalIlluminationRayCount{};
    std::vector<GpuRadianceCascadeLevel> radianceCascadeLevels;
    std::uint64_t radianceCascadeRadianceBytes{}; // two RGBA16F ping-pong buffers
    std::uint64_t radianceCascadeProbeBytes{};    // all 32-byte probe records
    std::vector<VoxelLightingDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] VoxelLightingFramePlan make_voxel_lighting_frame_plan(
    std::uint32_t width, std::uint32_t height, const RenderEnvironment& environment,
    const GpuRadianceCascadeSettings& cascades = {},
    float metersPerVoxel = kDefaultMetersPerVoxel);

} // namespace dve::render
