#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dve/camera_system.hpp"

namespace dve::render {

inline constexpr std::uint32_t kMaximumShadowCascades = 4U;

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

[[nodiscard]] CascadedShadowPlan make_cascaded_shadow_plan(const camera::CameraPose& camera,
                                                            Float3 directionTowardSun,
                                                            const CascadedShadowSettings& settings = {});
[[nodiscard]] CascadeSelection select_shadow_cascade(const CascadedShadowPlan& plan,
                                                     float viewDepthMeters) noexcept;
[[nodiscard]] std::vector<std::uint32_t> shadow_cascades_intersecting_sphere(
    const CascadedShadowPlan& plan, Float3 center, float radiusMeters);

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
