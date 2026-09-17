#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dve/asset_cooker.hpp"

namespace dve {

enum class MaterialLayerSemantic : std::uint8_t {
    Custom,
    Paint,
    Rust,
    Dirt,
    Wetness,
    Snow,
    Scorch,
    FractureExposure,
};

enum class MaterialLayerOpacityPolicy : std::uint8_t {
    PreserveBase,
    Lerp,
    Multiply,
    Maximum,
    Replace,
};

struct MaterialSurfaceSample {
    Float4 baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Float3 normal{0.0F, 0.0F, 1.0F};
    float metallic{};
    // DVE stores perceptual roughness, matching the editor/material scalar convention.
    float roughness{1.0F};
    Float3 emissive{};
    float opacity{1.0F};
    float height{0.5F};
};

struct MaterialLayerSample {
    MaterialSurfaceSample surface{};
    MaterialLayerSemantic semantic{MaterialLayerSemantic::Custom};
    MaterialLayerBlendMode baseColorBlend{MaterialLayerBlendMode::Lerp};
    MaterialLayerOpacityPolicy opacityPolicy{MaterialLayerOpacityPolicy::PreserveBase};
    float mask{1.0F};
    float authoredWeight{1.0F};
    float heightBlendStrength{1.0F};
    float heightBlendBias{};
    float heightBlendTransition{0.1F};
    bool enabled{true};
};

struct MaterialLayerCompositeStats {
    std::uint32_t consideredLayers{};
    std::uint32_t appliedLayers{};
    float lastCoverage{};
    float maximumCoverage{};
};

[[nodiscard]] const char* material_layer_semantic_label(MaterialLayerSemantic semantic) noexcept;
[[nodiscard]] const char* material_layer_opacity_policy_label(MaterialLayerOpacityPolicy policy) noexcept;

[[nodiscard]] bool validate_material_layer_sample(
    const MaterialLayerSample& layer,
    std::string* error = nullptr) noexcept;

// Converts the authored mask and relative surface heights into a stable [0,1] coverage value.
// A zero heightBlendStrength reduces exactly to mask*weight.
[[nodiscard]] float height_aware_material_layer_coverage(
    float baseHeight,
    const MaterialLayerSample& layer) noexcept;

// Reoriented normal mapping. The layer normal is interpreted as detail relative to +Z and is
// reoriented onto the current base normal before interpolation.
[[nodiscard]] Float3 blend_reoriented_material_normals(
    Float3 baseNormal,
    Float3 layerNormal,
    float coverage) noexcept;

// Applies a tangent-space detail normal to an arbitrary world-space frame using the same RNM
// convention. This is the renderer-facing form used by layered polygon materials and decals.
[[nodiscard]] Float3 apply_reoriented_tangent_material_normal(
    Float3 baseWorldNormal,
    Float3 tangent,
    Float3 bitangent,
    Float3 tangentDetailNormal,
    float coverage) noexcept;

[[nodiscard]] MaterialSurfaceSample blend_material_surface_layer(
    const MaterialSurfaceSample& base,
    const MaterialLayerSample& layer,
    float* appliedCoverage = nullptr) noexcept;

[[nodiscard]] MaterialSurfaceSample composite_material_surface_layers(
    MaterialSurfaceSample base,
    std::span<const MaterialLayerSample> layers,
    MaterialLayerCompositeStats* stats = nullptr) noexcept;

} // namespace dve
