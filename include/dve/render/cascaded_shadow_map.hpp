#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dve/camera_system.hpp"

namespace dve::render {

inline constexpr std::uint32_t kMaximumShadowCascades = 4U;

// How cascaded_shadow_resolve.hlsl turns the depth atlases into sun visibility.
enum class CascadedSoftShadowFilter : std::uint32_t {
    // Fixed (2r+1)^2 comparison kernel per layer; the darker of the static and dynamic layers.
    // Softness does not follow the sun's size or the blocker distance, and two penumbrae never
    // combine (no shadow blister).
    Pcf = 0,
    // Bitmask soft shadows: every texel in front of the receiver is projected back onto the sun
    // disc and the covered disc samples are OR-ed across texels and both layers, so penumbra width
    // follows the sun's size and blocker distance and overlapping penumbrae merge as the union of
    // what each occluder hides (the shadow blister). See bitmask_soft_shadow.hpp.
    Bitmask = 1,
};

struct CascadedShadowSettings {
    std::uint32_t cascadeCount{4};
    std::uint32_t cascadeResolution{2048};
    float maximumDistanceMeters{250.0F};
    float splitLambda{0.65F};
    float blendFraction{0.10F};
    float depthPaddingMeters{40.0F};
    float receiverBiasMeters{0.01F};
    float normalBiasMeters{0.025F};
    std::uint32_t pcfRadius{1};
    CascadedSoftShadowFilter softShadowFilter{CascadedSoftShadowFilter::Bitmask};
    // Bitmask only: how far (texels) blockers are looked for; caps the penumbra at about twice
    // this many texels. 1..16. Cost grows with the square.
    std::uint32_t softShadowSearchTexels{6};
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct ShadowAtlasUvBounds {
    float minimumU{};
    float minimumV{};
    float maximumU{};
    float maximumV{};
};

struct ShadowAtlasViewport {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct CascadedShadowCascade {
    std::uint32_t index{};
    float splitNearMeters{};
    float splitFarMeters{};
    float blendStartMeters{};
    Float3 center{};
    Float3 snappedCenter{};
    Float3 lightRight{1.0F, 0.0F, 0.0F};
    Float3 lightUp{0.0F, 1.0F, 0.0F};
    Float3 lightForward{0.0F, 0.0F, 1.0F};
    float radiusMeters{};
    float texelSizeMeters{};
    float minimumLightDepth{};
    float maximumLightDepth{};
    ShadowAtlasViewport atlas{};
};

struct CascadedShadowPlan {
    CascadedShadowSettings settings{};
    std::uint32_t atlasWidth{};
    std::uint32_t atlasHeight{};
    std::vector<CascadedShadowCascade> cascades;
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct CascadeSelection {
    std::uint32_t primaryCascade{};
    std::uint32_t secondaryCascade{};
    float secondaryWeight{};
    bool beyondShadowDistance{};
};

// cbuffer CascadedShadowConstants (b9) of cascaded_shadow_resolve.hlsl, field for field.
struct GpuCascadedShadowConstants {
    float cascadeSplitFarMeters[4]{};
    float cascadeBlendStartMeters[4]{};
    float cascadeTexelSize[2]{};
    std::uint32_t cascadeCount{};
    std::uint32_t cascadePcfRadius{};
    std::uint32_t cascadedShadowElementCount{};
    std::uint32_t cascadeSoftShadowFilter{};
    std::uint32_t cascadeSoftShadowSearchTexels{};
    float cascadeTanSunAngularRadius{};
    float cascadeTexelMeters[4]{};
    float cascadeDepthRangeMeters[4]{};
};
static_assert(sizeof(GpuCascadedShadowConstants) == 96U, "matches the HLSL cbuffer");

[[nodiscard]] CascadedShadowPlan make_cascaded_shadow_plan(const camera::CameraPose& camera,
                                                            Float3 directionTowardSun,
                                                            const CascadedShadowSettings& settings = {});
[[nodiscard]] CascadeSelection select_shadow_cascade(const CascadedShadowPlan& plan,
                                                     float viewDepthMeters) noexcept;
[[nodiscard]] std::vector<std::uint32_t> shadow_cascades_intersecting_sphere(
    const CascadedShadowPlan& plan, Float3 center, float radiusMeters);

// Constants for one resolve dispatch. sunAngularRadius is RenderEnvironment::shadowSoftnessRadians.
[[nodiscard]] GpuCascadedShadowConstants make_gpu_cascaded_shadow_constants(
    const CascadedShadowPlan& plan, std::uint32_t elementCount, float sunAngularRadius) noexcept;

[[nodiscard]] ShadowAtlasUvBounds cascaded_shadow_atlas_uv_bounds(
    const CascadedShadowPlan& plan, std::uint32_t cascadeIndex) noexcept;
// Vulkan clip coordinate written by the live cascade caster. X/Y are relative to the
// snapped cascade centre; Z uses the same absolute light-space origin as the sampler.
[[nodiscard]] Float3 cascaded_shadow_caster_clip_coordinate(
    const CascadedShadowCascade& cascade, Float3 worldPosition) noexcept;
[[nodiscard]] Float3 cascaded_shadow_atlas_coordinate(
    const CascadedShadowPlan& plan,
    std::uint32_t cascadeIndex,
    Float3 worldPosition) noexcept;
[[nodiscard]] Float3 cascaded_shadow_atlas_coordinate(
    const CascadedShadowPlan& plan,
    std::uint32_t cascadeIndex,
    Float3 worldPosition,
    Float3 geometricNormal) noexcept;


} // namespace dve::render
