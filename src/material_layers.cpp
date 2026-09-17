#include "dve/material_layers.hpp"

#include <algorithm>
#include <cmath>

namespace dve {
namespace {

[[nodiscard]] float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
[[nodiscard]] bool finite(Float4 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

[[nodiscard]] float dot3(Float3 a, Float3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] Float3 normalize3(Float3 value, Float3 fallback = {0.0F, 0.0F, 1.0F}) noexcept {
    const float squared = dot3(value, value);
    if (!(squared > 1.0e-20F) || !finite(squared)) return fallback;
    const float inverse = 1.0F / std::sqrt(squared);
    return {value.x * inverse, value.y * inverse, value.z * inverse};
}

[[nodiscard]] float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] Float3 lerp(Float3 a, Float3 b, float t) noexcept {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}
[[nodiscard]] Float4 lerp(Float4 a, Float4 b, float t) noexcept {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)};
}

[[nodiscard]] Float4 blend_base_color(
    Float4 base,
    Float4 layer,
    float coverage,
    MaterialLayerBlendMode mode) noexcept {
    switch (mode) {
        case MaterialLayerBlendMode::Lerp:
            return lerp(base, layer, coverage);
        case MaterialLayerBlendMode::Multiply:
            return {
                lerp(base.x, base.x * layer.x, coverage),
                lerp(base.y, base.y * layer.y, coverage),
                lerp(base.z, base.z * layer.z, coverage),
                base.w,
            };
        case MaterialLayerBlendMode::Additive:
            return {
                std::max(0.0F, base.x + layer.x * coverage),
                std::max(0.0F, base.y + layer.y * coverage),
                std::max(0.0F, base.z + layer.z * coverage),
                base.w,
            };
    }
    return base;
}

} // namespace

const char* material_layer_semantic_label(MaterialLayerSemantic semantic) noexcept {
    switch (semantic) {
        case MaterialLayerSemantic::Custom: return "Custom";
        case MaterialLayerSemantic::Paint: return "Paint";
        case MaterialLayerSemantic::Rust: return "Rust";
        case MaterialLayerSemantic::Dirt: return "Dirt";
        case MaterialLayerSemantic::Wetness: return "Wetness";
        case MaterialLayerSemantic::Snow: return "Snow";
        case MaterialLayerSemantic::Scorch: return "Scorch";
        case MaterialLayerSemantic::FractureExposure: return "Fracture Exposure";
    }
    return "Unknown";
}

const char* material_layer_opacity_policy_label(MaterialLayerOpacityPolicy policy) noexcept {
    switch (policy) {
        case MaterialLayerOpacityPolicy::PreserveBase: return "Preserve Base";
        case MaterialLayerOpacityPolicy::Lerp: return "Lerp";
        case MaterialLayerOpacityPolicy::Multiply: return "Multiply";
        case MaterialLayerOpacityPolicy::Maximum: return "Maximum";
        case MaterialLayerOpacityPolicy::Replace: return "Replace";
    }
    return "Unknown";
}

bool validate_material_layer_sample(const MaterialLayerSample& layer, std::string* error) noexcept {
    auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (static_cast<unsigned>(layer.semantic) >
        static_cast<unsigned>(MaterialLayerSemantic::FractureExposure))
        return fail("material layer semantic is invalid");
    if (static_cast<unsigned>(layer.baseColorBlend) >
        static_cast<unsigned>(MaterialLayerBlendMode::Additive))
        return fail("material layer base-color blend mode is invalid");
    if (static_cast<unsigned>(layer.opacityPolicy) >
        static_cast<unsigned>(MaterialLayerOpacityPolicy::Replace))
        return fail("material layer opacity policy is invalid");
    if (!finite(layer.surface.baseColor) || !finite(layer.surface.normal) ||
        !finite(layer.surface.metallic) || !finite(layer.surface.roughness) ||
        !finite(layer.surface.emissive) || !finite(layer.surface.opacity) ||
        !finite(layer.surface.height))
        return fail("material layer surface contains a non-finite value");
    if (!finite(layer.mask) || !finite(layer.authoredWeight) ||
        !finite(layer.heightBlendStrength) || !finite(layer.heightBlendBias) ||
        !finite(layer.heightBlendTransition))
        return fail("material layer controls contain a non-finite value");
    if (layer.mask < 0.0F || layer.mask > 1.0F ||
        layer.authoredWeight < 0.0F || layer.authoredWeight > 1.0F)
        return fail("material layer mask and authored weight must be in [0,1]");
    if (layer.heightBlendStrength < 0.0F || layer.heightBlendTransition < 0.0F)
        return fail("material layer height controls must be non-negative");
    return true;
}

float height_aware_material_layer_coverage(
    float baseHeight,
    const MaterialLayerSample& layer) noexcept {
    if (!layer.enabled) return 0.0F;
    const float mask = saturate(layer.mask) * saturate(layer.authoredWeight);
    if (!(layer.heightBlendStrength > 0.0F)) return mask;

    const float relativeHeight =
        (layer.surface.height - baseHeight) * layer.heightBlendStrength + layer.heightBlendBias;
    const float transition = std::max(1.0e-5F, layer.heightBlendTransition);
    // The mask remains authoritative. Height shifts the boundary rather than inventing coverage
    // where the authored mask is zero.
    const float boundary = saturate(0.5F + relativeHeight / transition);
    return saturate(mask * boundary * 2.0F);
}

Float3 blend_reoriented_material_normals(
    Float3 baseNormal,
    Float3 layerNormal,
    float coverage) noexcept {
    const Float3 base = normalize3(baseNormal);
    const Float3 detail = normalize3(layerNormal);

    // Stable RNM form. An identity detail normal (0,0,1) returns the base normal exactly.
    const Float3 t{base.x, base.y, base.z + 1.0F};
    const Float3 u{-detail.x, -detail.y, detail.z};
    const float tZ = std::max(1.0e-6F, t.z);
    const float projection = dot3(t, u) / tZ;
    const Float3 reoriented = normalize3({
        t.x * projection - u.x,
        t.y * projection - u.y,
        t.z * projection - u.z,
    }, base);
    return normalize3(lerp(base, reoriented, saturate(coverage)), base);
}

Float3 apply_reoriented_tangent_material_normal(
    Float3 baseWorldNormal,
    Float3 tangent,
    Float3 bitangent,
    Float3 tangentDetailNormal,
    float coverage) noexcept {
    const Float3 base = normalize3(baseWorldNormal);
    const Float3 tangentAxis = normalize3(tangent, {1.0F, 0.0F, 0.0F});
    const Float3 bitangentAxis = normalize3(bitangent, {0.0F, 1.0F, 0.0F});
    const Float3 detail = blend_reoriented_material_normals(
        {0.0F, 0.0F, 1.0F}, tangentDetailNormal, coverage);
    return normalize3({
        tangentAxis.x * detail.x + bitangentAxis.x * detail.y + base.x * detail.z,
        tangentAxis.y * detail.x + bitangentAxis.y * detail.y + base.y * detail.z,
        tangentAxis.z * detail.x + bitangentAxis.z * detail.y + base.z * detail.z,
    }, base);
}

MaterialSurfaceSample blend_material_surface_layer(
    const MaterialSurfaceSample& base,
    const MaterialLayerSample& layer,
    float* appliedCoverage) noexcept {
    const float coverage = height_aware_material_layer_coverage(base.height, layer);
    if (appliedCoverage != nullptr) *appliedCoverage = coverage;
    if (!(coverage > 0.0F)) return base;

    MaterialSurfaceSample output = base;
    output.baseColor = blend_base_color(base.baseColor, layer.surface.baseColor,
                                        coverage, layer.baseColorBlend);
    output.normal = blend_reoriented_material_normals(base.normal, layer.surface.normal, coverage);

    // Roughness is stored perceptually, so interpolation is intentionally performed directly in
    // perceptual-roughness space rather than on squared microfacet alpha.
    output.roughness = saturate(lerp(base.roughness, layer.surface.roughness, coverage));
    output.metallic = saturate(lerp(base.metallic, layer.surface.metallic, coverage));
    output.emissive = {
        std::max(0.0F, base.emissive.x + layer.surface.emissive.x * coverage),
        std::max(0.0F, base.emissive.y + layer.surface.emissive.y * coverage),
        std::max(0.0F, base.emissive.z + layer.surface.emissive.z * coverage),
    };

    switch (layer.opacityPolicy) {
        case MaterialLayerOpacityPolicy::PreserveBase:
            output.opacity = base.opacity;
            break;
        case MaterialLayerOpacityPolicy::Lerp:
            output.opacity = saturate(lerp(base.opacity, layer.surface.opacity, coverage));
            break;
        case MaterialLayerOpacityPolicy::Multiply:
            output.opacity = saturate(lerp(base.opacity,
                                           base.opacity * layer.surface.opacity,
                                           coverage));
            break;
        case MaterialLayerOpacityPolicy::Maximum:
            output.opacity = saturate(lerp(base.opacity,
                                           std::max(base.opacity, layer.surface.opacity),
                                           coverage));
            break;
        case MaterialLayerOpacityPolicy::Replace:
            output.opacity = saturate(layer.surface.opacity);
            break;
    }
    output.baseColor.w = output.opacity;
    output.height = lerp(base.height, layer.surface.height, coverage);
    return output;
}

MaterialSurfaceSample composite_material_surface_layers(
    MaterialSurfaceSample base,
    std::span<const MaterialLayerSample> layers,
    MaterialLayerCompositeStats* stats) noexcept {
    MaterialLayerCompositeStats local{};
    for (const MaterialLayerSample& layer : layers) {
        ++local.consideredLayers;
        float coverage{};
        const MaterialSurfaceSample next = blend_material_surface_layer(base, layer, &coverage);
        local.lastCoverage = coverage;
        local.maximumCoverage = std::max(local.maximumCoverage, coverage);
        if (coverage > 0.0F) {
            ++local.appliedLayers;
            base = next;
        }
    }
    if (stats != nullptr) *stats = local;
    return base;
}

} // namespace dve
