#include "dve/gpu_render_environment.hpp"

#include <cmath>

namespace dve {
namespace {
static_assert(static_cast<std::uint32_t>(TonemapOperator::ACES) == 0);
static_assert(static_cast<std::uint32_t>(TonemapOperator::Reinhard) == 1);
static_assert(static_cast<std::uint32_t>(TonemapOperator::Clamp) == 2);
static_assert(static_cast<std::uint32_t>(GlobalIlluminationMode::Off) == 0);
static_assert(static_cast<std::uint32_t>(GlobalIlluminationMode::AmbientHemisphere) == 1);
static_assert(static_cast<std::uint32_t>(GlobalIlluminationMode::VoxelOneBounce) == 2);
static_assert(static_cast<std::uint32_t>(ShadowMode::Off) == 0);
static_assert(static_cast<std::uint32_t>(ShadowMode::Hard) == 1);
static_assert(static_cast<std::uint32_t>(ShadowMode::Soft) == 2);
static_assert(static_cast<std::uint32_t>(ShadowMode::Contact) == 3);
static_assert(static_cast<std::uint32_t>(ShadowMode::Hybrid) == 4);

Float3 safe_normalize(Float3 v) noexcept {
    const float lengthSquared = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!(lengthSquared > 0.0F)) return {0.0F, 1.0F, 0.0F};
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {v.x * inverseLength, v.y * inverseLength, v.z * inverseLength};
}
} // namespace

GpuRenderEnvironment pack_gpu_render_environment(
    const RenderEnvironment& environment, std::uint32_t imageWidth, std::uint32_t imageHeight,
    float metersPerVoxel) noexcept {
    GpuRenderEnvironment gpu;
    const Float3 sunDirection = safe_normalize(environment.sunDirection);
    gpu.sunDirectionX = sunDirection.x;
    gpu.sunDirectionY = sunDirection.y;
    gpu.sunDirectionZ = sunDirection.z;
    gpu.sunIntensity = environment.sunIntensity;
    gpu.sunColorR = environment.sunColor.x;
    gpu.sunColorG = environment.sunColor.y;
    gpu.sunColorB = environment.sunColor.z;
    gpu.subsurfaceMaxDistanceMeters = environment.subsurfaceMaxDistanceMeters;
    gpu.skyColorR = environment.skyColor.x;
    gpu.skyColorG = environment.skyColor.y;
    gpu.skyColorB = environment.skyColor.z;
    gpu.exposure = environment.exposure;
    gpu.groundColorR = environment.groundColor.x;
    gpu.groundColorG = environment.groundColor.y;
    gpu.groundColorB = environment.groundColor.z;
    gpu.tonemapOperator = static_cast<std::uint32_t>(environment.tonemapOperator);
    gpu.globalTintR = environment.globalTint.x;
    gpu.globalTintG = environment.globalTint.y;
    gpu.globalTintB = environment.globalTint.z;
    gpu.bloomThreshold = environment.bloomThreshold;
    gpu.bloomIntensity = environment.bloomIntensity;
    gpu.bloomRadius = environment.bloomRadius;
    gpu.imageWidth = imageWidth;
    gpu.imageHeight = imageHeight;
    gpu.globalIlluminationMode = static_cast<std::uint32_t>(environment.globalIlluminationMode);
    gpu.globalIlluminationIntensity = environment.globalIlluminationIntensity;
    gpu.globalIlluminationMaxDistanceMeters = environment.globalIlluminationMaxDistanceMeters;
    gpu.globalIlluminationSamples = environment.globalIlluminationSamples;
    gpu.shadowMode = static_cast<std::uint32_t>(environment.shadowMode);
    gpu.shadowStrength = environment.shadowStrength;
    gpu.shadowSoftnessRadians = environment.shadowSoftnessRadians;
    gpu.shadowSamples = environment.shadowSamples;
    gpu.shadowMaxDistanceMeters = environment.shadowMaxDistanceMeters;
    gpu.contactShadowDistanceMeters = environment.contactShadowDistanceMeters;
    gpu.shadowBiasMeters = environment.shadowBiasMeters;
    gpu.metersPerVoxel = std::isfinite(metersPerVoxel) && metersPerVoxel > 0.0F
        ? metersPerVoxel : 0.10F;
    return gpu;
}

} // namespace dve
