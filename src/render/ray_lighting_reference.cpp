#include "dve/render/ray_lighting_reference.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::render {
namespace {
constexpr float kPi = 3.14159265358979323846F;

float dot(Float3 a, Float3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
Float3 normalize(Float3 value) noexcept {
    const float lengthSquared = dot(value, value);
    if (!(lengthSquared > 1.0e-12F)) return {0.0F, 1.0F, 0.0F};
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {value.x*inverseLength, value.y*inverseLength, value.z*inverseLength};
}
Float3 add(Float3 a, Float3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Float3 multiply(Float3 a, Float3 b) noexcept { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
Float3 scale(Float3 a, float value) noexcept { return {a.x*value,a.y*value,a.z*value}; }
Float3 blend(Float3 a, Float3 b, float weight) noexcept {
    return add(scale(a, 1.0F-weight), scale(b, weight));
}
float fract(float value) noexcept { return value - std::floor(value); }

float radical_inverse_base2(std::uint32_t bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}
}

bool LightingTemporalSettings::validate(std::string* error) const noexcept {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!std::isfinite(historyWeight) || historyWeight < 0.0F || historyWeight > 1.0F)
        return fail("historyWeight must be finite and in [0,1]");
    if (!std::isfinite(depthToleranceMeters) || depthToleranceMeters < 0.0F)
        return fail("depthToleranceMeters must be finite and non-negative");
    if (!std::isfinite(normalThreshold) || normalThreshold < 0.0F || normalThreshold > 1.0F)
        return fail("normalThreshold must be finite and in [0,1]");
    if (maximumHistory == 0U || maximumHistory > 4096U)
        return fail("maximumHistory must be in [1,4096]");
    return true;
}

float meters_to_voxel_units(float meters, float metersPerVoxel) noexcept {
    if (!std::isfinite(meters) || !std::isfinite(metersPerVoxel) ||
        meters <= 0.0F || metersPerVoxel <= 0.0F) return 0.0F;
    return meters / metersPerVoxel;
}

float voxel_units_to_meters(float voxels, float metersPerVoxel) noexcept {
    if (!std::isfinite(voxels) || !std::isfinite(metersPerVoxel) ||
        voxels <= 0.0F || metersPerVoxel <= 0.0F) return 0.0F;
    return voxels * metersPerVoxel;
}

float distance_weighted_ao_visibility(
    bool hit, float hitDistanceVoxels, float maximumDistanceMeters,
    float metersPerVoxel) noexcept {
    if (!hit) return 1.0F;
    const float maximumVoxels = meters_to_voxel_units(maximumDistanceMeters, metersPerVoxel);
    if (!(maximumVoxels > 0.0F)) return 1.0F;
    const float proximity = 1.0F - std::clamp(hitDistanceVoxels / maximumVoxels, 0.0F, 1.0F);
    return 1.0F - proximity * proximity;
}

Float2 hammersley_2d(std::uint32_t sampleIndex, std::uint32_t sampleCount) noexcept {
    const std::uint32_t count = std::max(sampleCount, 1U);
    return {(static_cast<float>(sampleIndex) + 0.5F) / static_cast<float>(count),
            radical_inverse_base2(sampleIndex)};
}

Float2 rotated_hammersley_2d(
    std::uint32_t sampleIndex, std::uint32_t sampleCount, Float2 rotation) noexcept {
    const Float2 sample = hammersley_2d(sampleIndex, sampleCount);
    return {fract(sample.x + rotation.x), fract(sample.y + rotation.y)};
}

Float3 one_bounce_diffuse_radiance(
    Float3 baseColor, float metallic, Float3 emissive, Float3 ambientRadiance,
    Float3 sunColor, float sunIntensity, Float3 bounceNormal,
    Float3 sunDirection, float sunVisibility) noexcept {
    const Float3 normal = normalize(bounceNormal);
    const Float3 light = normalize(sunDirection);
    const float ndotl = std::max(0.0F, dot(normal, light));
    const Float3 directSun = scale(sunColor,
        std::max(0.0F, sunIntensity) * ndotl * std::clamp(sunVisibility, 0.0F, 1.0F) / kPi);
    const Float3 incident = add(ambientRadiance, directSun);
    return add(emissive, scale(multiply(baseColor, incident),
                               1.0F - std::clamp(metallic, 0.0F, 1.0F)));
}

bool lighting_history_compatible(
    const LightingTemporalSample& current, const LightingTemporalSample& history,
    const LightingTemporalSettings& settings) noexcept {
    if (!settings.validate()) return false;
    if (!std::isfinite(current.depthMeters) || !std::isfinite(history.depthMeters)) return false;
    if (std::abs(current.depthMeters - history.depthMeters) > settings.depthToleranceMeters)
        return false;
    return dot(normalize(current.normal), normalize(history.normal)) >= settings.normalThreshold;
}

LightingTemporalSample accumulate_lighting_history(
    const LightingTemporalSample& current, const LightingTemporalSample& history,
    const LightingTemporalSettings& settings, bool resetHistory) noexcept {
    LightingTemporalSample result = current;
    const bool accept = !resetHistory && lighting_history_compatible(current, history, settings);
    const std::uint32_t oldLength = accept ? history.historyLength : 0U;
    result.historyLength = std::min(oldLength + 1U, std::max(settings.maximumHistory, 1U));
    const float weight = accept
        ? std::clamp(settings.historyWeight, 0.0F, 1.0F) *
          static_cast<float>(oldLength) / static_cast<float>(std::max(oldLength + 1U, 1U))
        : 0.0F;
    result.indirectDiffuse = blend(current.indirectDiffuse, history.indirectDiffuse, weight);
    result.ambientOcclusion = std::lerp(
        std::clamp(current.ambientOcclusion, 0.0F, 1.0F),
        std::clamp(history.ambientOcclusion, 0.0F, 1.0F), weight);
    result.shadowVisibility = std::lerp(
        std::clamp(current.shadowVisibility, 0.0F, 1.0F),
        std::clamp(history.shadowVisibility, 0.0F, 1.0F), weight);
    return result;
}

} // namespace dve::render
