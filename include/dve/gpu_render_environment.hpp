#pragma once

#include <cstddef>
#include <cstdint>

#include "dve/render_environment.hpp"

namespace dve {

// Byte-exact mirror of shaders/common/render_environment.hlsli. Fields are ordered as nine
// float4-sized cbuffer registers. The final scalar is the world scale used to convert authored
// metre distances into the voxel-index units consumed by TraceVoxelRay. Offset tests intentionally
// verify every field.
struct GpuRenderEnvironment {
    float sunDirectionX{0.0F}, sunDirectionY{0.0F}, sunDirectionZ{0.0F};
    float sunIntensity{0.0F};
    float sunColorR{0.0F}, sunColorG{0.0F}, sunColorB{0.0F};
    float subsurfaceMaxDistanceMeters{0.0F};
    float skyColorR{0.0F}, skyColorG{0.0F}, skyColorB{0.0F};
    float exposure{1.0F};
    float groundColorR{0.0F}, groundColorG{0.0F}, groundColorB{0.0F};
    std::uint32_t tonemapOperator{0};
    float globalTintR{1.0F}, globalTintG{1.0F}, globalTintB{1.0F};
    float bloomThreshold{1.0F};
    float bloomIntensity{0.0F};
    float bloomRadius{0.0F};
    std::uint32_t imageWidth{0};
    std::uint32_t imageHeight{0};

    std::uint32_t globalIlluminationMode{0};
    float globalIlluminationIntensity{0.0F};
    float globalIlluminationMaxDistanceMeters{0.0F};
    std::uint32_t globalIlluminationSamples{0};

    std::uint32_t shadowMode{0};
    float shadowStrength{0.0F};
    float shadowSoftnessRadians{0.0F};
    std::uint32_t shadowSamples{0};

    float shadowMaxDistanceMeters{0.0F};
    float contactShadowDistanceMeters{0.0F};
    float shadowBiasMeters{0.0F};
    float metersPerVoxel{0.10F};
};

static_assert(sizeof(GpuRenderEnvironment) == 36 * sizeof(std::uint32_t));
static_assert(alignof(GpuRenderEnvironment) == 4);

[[nodiscard]] GpuRenderEnvironment pack_gpu_render_environment(
    const RenderEnvironment& environment, std::uint32_t imageWidth, std::uint32_t imageHeight,
    float metersPerVoxel) noexcept;

} // namespace dve
