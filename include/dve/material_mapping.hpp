#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "dve/asset_cooker.hpp"

namespace dve {

enum class MaterialMappingMode : std::uint8_t {
    UV0,
    UV1,
    WorldTriplanar,
    ObjectTriplanar,
};

enum class HeightMappingMode : std::uint8_t {
    Off,
    OffsetParallax,
    SteepParallax,
    ParallaxOcclusion,
};

struct TextureTransform2D {
    Float2 scale{1.0F, 1.0F};
    Float2 offset{};
    float rotationRadians{};
};

struct MaterialMappingSettings {
    MaterialMappingMode mappingMode{MaterialMappingMode::UV0};
    TextureTransform2D baseTransform{};

    float triplanarScale{1.0F};
    float triplanarBlendSharpness{4.0F};

    TextureTransform2D detailTransform{{8.0F, 8.0F}, {}, 0.0F};
    float detailColorStrength{};
    float detailNormalStrength{1.0F};
    float detailRoughnessStrength{};
    float detailFadeStartMeters{2.0F};
    float detailFadeEndMeters{20.0F};

    HeightMappingMode heightMode{HeightMappingMode::Off};
    float heightScale{0.04F};
    float heightReferencePlane{0.5F};
    std::uint32_t minimumHeightSteps{8U};
    std::uint32_t maximumHeightSteps{32U};
    std::uint32_t refinementSteps{4U};
    float maximumParallaxDistanceMeters{20.0F};
};

struct TriplanarCoordinates {
    Float2 xProjection{}; // YZ
    Float2 yProjection{}; // XZ
    Float2 zProjection{}; // XY
    Float3 weights{};
};

struct ParallaxMappingResult {
    Float2 uv{};
    std::uint32_t samples{};
    bool valid{true};
};

using HeightSampleFunction = std::function<float(Float2)>;

[[nodiscard]] bool validate_material_mapping_settings(
    const MaterialMappingSettings& settings,
    std::string* error = nullptr) noexcept;

[[nodiscard]] Float2 transform_texture_coordinates(
    Float2 uv,
    const TextureTransform2D& transform) noexcept;

[[nodiscard]] TriplanarCoordinates make_triplanar_coordinates(
    Float3 position,
    Float3 surfaceNormal,
    float scale,
    float blendSharpness) noexcept;

[[nodiscard]] float material_detail_fade(
    float cameraDistanceMeters,
    const MaterialMappingSettings& settings) noexcept;

[[nodiscard]] ParallaxMappingResult apply_parallax_mapping(
    Float2 uv,
    Float3 tangentSpaceViewDirection,
    float cameraDistanceMeters,
    const MaterialMappingSettings& settings,
    const HeightSampleFunction& sampleHeight);

[[nodiscard]] Float3 blend_detail_normal(
    Float3 baseNormal,
    Float3 detailNormal,
    float strength) noexcept;

[[nodiscard]] Float4 blend_detail_color(
    Float4 baseColor,
    Float4 detailColor,
    float strength) noexcept;

[[nodiscard]] float blend_detail_roughness(
    float baseRoughness,
    float detailRoughness,
    float strength) noexcept;

} // namespace dve
