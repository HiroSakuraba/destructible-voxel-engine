#pragma once

#include <cstdint>
#include <string>

#include "dve/types.hpp"

namespace dve::render {

struct Float2 {
    float x{};
    float y{};
};

struct LightingTemporalSettings {
    float historyWeight{0.90F};
    float depthToleranceMeters{0.05F};
    float normalThreshold{0.85F};
    std::uint32_t maximumHistory{32U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct LightingTemporalSample {
    Float3 indirectDiffuse{};
    float ambientOcclusion{1.0F};
    float shadowVisibility{1.0F};
    float depthMeters{};
    Float3 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t historyLength{};
};

[[nodiscard]] float meters_to_voxel_units(float meters, float metersPerVoxel) noexcept;
[[nodiscard]] float voxel_units_to_meters(float voxels, float metersPerVoxel) noexcept;
[[nodiscard]] float distance_weighted_ao_visibility(
    bool hit, float hitDistanceVoxels, float maximumDistanceMeters,
    float metersPerVoxel) noexcept;
[[nodiscard]] Float2 hammersley_2d(
    std::uint32_t sampleIndex, std::uint32_t sampleCount) noexcept;
[[nodiscard]] Float2 rotated_hammersley_2d(
    std::uint32_t sampleIndex, std::uint32_t sampleCount,
    Float2 rotation) noexcept;
[[nodiscard]] Float3 one_bounce_diffuse_radiance(
    Float3 baseColor, float metallic, Float3 emissive, Float3 ambientRadiance,
    Float3 sunColor, float sunIntensity, Float3 bounceNormal,
    Float3 sunDirection, float sunVisibility) noexcept;
[[nodiscard]] bool lighting_history_compatible(
    const LightingTemporalSample& current, const LightingTemporalSample& history,
    const LightingTemporalSettings& settings) noexcept;
[[nodiscard]] LightingTemporalSample accumulate_lighting_history(
    const LightingTemporalSample& current, const LightingTemporalSample& history,
    const LightingTemporalSettings& settings, bool resetHistory = false) noexcept;

} // namespace dve::render
