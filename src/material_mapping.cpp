#include "dve/material_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve {
namespace {
constexpr float kEpsilon = 1.0e-6F;

bool finite(float value) noexcept { return std::isfinite(value); }
bool finite(Float2 value) noexcept { return finite(value.x) && finite(value.y); }

Float3 normalize3(Float3 value) noexcept {
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (!(lengthSquared > kEpsilon * kEpsilon) || !finite(lengthSquared)) {
        return {0.0F, 0.0F, 1.0F};
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
}

float saturate(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }

float sanitized_height(const HeightSampleFunction& sampleHeight, Float2 uv) {
    if (!sampleHeight) return 0.5F;
    const float value = sampleHeight(uv);
    return finite(value) ? saturate(value) : 0.5F;
}

std::uint32_t step_count(Float3 viewDirection, const MaterialMappingSettings& settings) noexcept {
    const float grazing = 1.0F - saturate(std::abs(viewDirection.z));
    const float interpolated = static_cast<float>(settings.minimumHeightSteps) +
        (static_cast<float>(settings.maximumHeightSteps - settings.minimumHeightSteps) * grazing);
    return std::clamp(static_cast<std::uint32_t>(std::ceil(interpolated)),
                      settings.minimumHeightSteps, settings.maximumHeightSteps);
}

} // namespace

bool validate_material_mapping_settings(const MaterialMappingSettings& settings,
                                        std::string* error) noexcept {
    const auto fail = [&](const char* message) noexcept {
        if (error != nullptr) *error = message;
        return false;
    };
    if (static_cast<std::uint8_t>(settings.mappingMode) >
            static_cast<std::uint8_t>(MaterialMappingMode::ObjectTriplanar) ||
        static_cast<std::uint8_t>(settings.heightMode) >
            static_cast<std::uint8_t>(HeightMappingMode::ParallaxOcclusion)) {
        return fail("material mapping enum is invalid");
    }
    if (!finite(settings.baseTransform.scale) || !finite(settings.baseTransform.offset) ||
        !finite(settings.baseTransform.rotationRadians) ||
        !finite(settings.detailTransform.scale) || !finite(settings.detailTransform.offset) ||
        !finite(settings.detailTransform.rotationRadians)) {
        return fail("texture transform contains a non-finite value");
    }
    if (std::abs(settings.baseTransform.scale.x) < kEpsilon ||
        std::abs(settings.baseTransform.scale.y) < kEpsilon ||
        std::abs(settings.detailTransform.scale.x) < kEpsilon ||
        std::abs(settings.detailTransform.scale.y) < kEpsilon) {
        return fail("texture transform scale must be nonzero");
    }
    if (!finite(settings.triplanarScale) || !(settings.triplanarScale > 0.0F) ||
        !finite(settings.triplanarBlendSharpness) ||
        settings.triplanarBlendSharpness < 1.0F || settings.triplanarBlendSharpness > 32.0F) {
        return fail("triplanar scale or blend sharpness is invalid");
    }
    if (!finite(settings.detailColorStrength) || settings.detailColorStrength < 0.0F ||
        settings.detailColorStrength > 2.0F ||
        !finite(settings.detailNormalStrength) || settings.detailNormalStrength < 0.0F ||
        settings.detailNormalStrength > 4.0F ||
        !finite(settings.detailRoughnessStrength) || settings.detailRoughnessStrength < 0.0F ||
        settings.detailRoughnessStrength > 2.0F ||
        !finite(settings.detailFadeStartMeters) || !finite(settings.detailFadeEndMeters) ||
        settings.detailFadeStartMeters < 0.0F ||
        settings.detailFadeEndMeters <= settings.detailFadeStartMeters) {
        return fail("detail mapping controls are invalid");
    }
    if (!finite(settings.heightScale) || settings.heightScale < 0.0F ||
        settings.heightScale > 1.0F ||
        !finite(settings.heightReferencePlane) || settings.heightReferencePlane < 0.0F ||
        settings.heightReferencePlane > 1.0F ||
        settings.minimumHeightSteps == 0U ||
        settings.maximumHeightSteps < settings.minimumHeightSteps ||
        settings.maximumHeightSteps > 128U || settings.refinementSteps > 12U ||
        !finite(settings.maximumParallaxDistanceMeters) ||
        settings.maximumParallaxDistanceMeters <= 0.0F) {
        return fail("height mapping controls are invalid");
    }
    if (settings.heightMode != HeightMappingMode::Off &&
        (settings.mappingMode == MaterialMappingMode::WorldTriplanar ||
         settings.mappingMode == MaterialMappingMode::ObjectTriplanar)) {
        return fail("parallax modes currently require UV0 or UV1 mapping");
    }
    return true;
}

Float2 transform_texture_coordinates(Float2 uv, const TextureTransform2D& transform) noexcept {
    const float cosine = std::cos(transform.rotationRadians);
    const float sine = std::sin(transform.rotationRadians);
    const Float2 scaled{uv.x * transform.scale.x, uv.y * transform.scale.y};
    return {
        cosine * scaled.x - sine * scaled.y + transform.offset.x,
        sine * scaled.x + cosine * scaled.y + transform.offset.y,
    };
}

TriplanarCoordinates make_triplanar_coordinates(Float3 position, Float3 surfaceNormal,
                                                float scale, float blendSharpness) noexcept {
    const Float3 normal = normalize3(surfaceNormal);
    Float3 weights{
        std::pow(std::abs(normal.x), blendSharpness),
        std::pow(std::abs(normal.y), blendSharpness),
        std::pow(std::abs(normal.z), blendSharpness),
    };
    const float sum = weights.x + weights.y + weights.z;
    if (!(sum > kEpsilon) || !finite(sum)) {
        weights = {0.0F, 0.0F, 1.0F};
    } else {
        weights = {weights.x / sum, weights.y / sum, weights.z / sum};
    }
    const float safeScale = finite(scale) && scale > kEpsilon ? scale : 1.0F;
    return {
        {position.z * safeScale, position.y * safeScale},
        {position.x * safeScale, position.z * safeScale},
        {position.x * safeScale, position.y * safeScale},
        weights,
    };
}

float material_detail_fade(float cameraDistanceMeters,
                           const MaterialMappingSettings& settings) noexcept {
    if (!finite(cameraDistanceMeters)) return 0.0F;
    if (cameraDistanceMeters <= settings.detailFadeStartMeters) return 1.0F;
    if (cameraDistanceMeters >= settings.detailFadeEndMeters) return 0.0F;
    const float alpha = (cameraDistanceMeters - settings.detailFadeStartMeters) /
        (settings.detailFadeEndMeters - settings.detailFadeStartMeters);
    const float smooth = alpha * alpha * (3.0F - 2.0F * alpha);
    return 1.0F - smooth;
}

ParallaxMappingResult apply_parallax_mapping(
    Float2 uv,
    Float3 tangentSpaceViewDirection,
    float cameraDistanceMeters,
    const MaterialMappingSettings& settings,
    const HeightSampleFunction& sampleHeight) {
    ParallaxMappingResult result{uv, 0U, true};
    if (settings.heightMode == HeightMappingMode::Off || settings.heightScale <= 0.0F ||
        cameraDistanceMeters >= settings.maximumParallaxDistanceMeters) {
        return result;
    }

    const Float3 view = normalize3(tangentSpaceViewDirection);
    const float viewZ = std::max(std::abs(view.z), 0.08F);
    const float distanceFade = saturate(1.0F - cameraDistanceMeters /
        settings.maximumParallaxDistanceMeters);
    const Float2 direction{
        (view.x / viewZ) * settings.heightScale * distanceFade,
        (view.y / viewZ) * settings.heightScale * distanceFade,
    };

    if (settings.heightMode == HeightMappingMode::OffsetParallax) {
        const float height = sanitized_height(sampleHeight, uv);
        result.uv = {
            uv.x - direction.x * (height - settings.heightReferencePlane),
            uv.y - direction.y * (height - settings.heightReferencePlane),
        };
        result.samples = 1U;
        return result;
    }

    const std::uint32_t steps = step_count(view, settings);
    const float layerDepth = 1.0F / static_cast<float>(steps);
    const Float2 delta{direction.x / static_cast<float>(steps),
                       direction.y / static_cast<float>(steps)};
    Float2 currentUv = uv;
    Float2 previousUv = uv;
    float currentLayerDepth = 0.0F;
    float previousLayerDepth = 0.0F;
    float currentHeight = sanitized_height(sampleHeight, currentUv);
    result.samples = 1U;

    while (currentLayerDepth < currentHeight && result.samples < steps) {
        previousUv = currentUv;
        previousLayerDepth = currentLayerDepth;
        currentUv = {currentUv.x - delta.x, currentUv.y - delta.y};
        currentLayerDepth += layerDepth;
        currentHeight = sanitized_height(sampleHeight, currentUv);
        ++result.samples;
    }

    if (settings.heightMode == HeightMappingMode::SteepParallax) {
        result.uv = currentUv;
        return result;
    }

    Float2 lowerUv = previousUv;
    Float2 upperUv = currentUv;
    float lowerDepth = previousLayerDepth;
    float upperDepth = currentLayerDepth;
    for (std::uint32_t iteration = 0; iteration < settings.refinementSteps; ++iteration) {
        const Float2 middleUv{(lowerUv.x + upperUv.x) * 0.5F,
                              (lowerUv.y + upperUv.y) * 0.5F};
        const float middleDepth = (lowerDepth + upperDepth) * 0.5F;
        const float middleHeight = sanitized_height(sampleHeight, middleUv);
        ++result.samples;
        if (middleDepth < middleHeight) {
            lowerUv = middleUv;
            lowerDepth = middleDepth;
        } else {
            upperUv = middleUv;
            upperDepth = middleDepth;
        }
    }
    result.uv = {(lowerUv.x + upperUv.x) * 0.5F,
                 (lowerUv.y + upperUv.y) * 0.5F};
    return result;
}

Float3 blend_detail_normal(Float3 baseNormal, Float3 detailNormal, float strength) noexcept {
    const Float3 base = normalize3(baseNormal);
    const Float3 detail = normalize3(detailNormal);
    const float amount = saturate(strength);
    // Whiteout-style normal blending is stable for tangent-space detail normals and avoids
    // flattening the base normal as a direct lerp would.
    const Float3 combined{
        base.x + detail.x * amount,
        base.y + detail.y * amount,
        base.z * std::max(0.0F, detail.z),
    };
    return normalize3(combined);
}

Float4 blend_detail_color(Float4 baseColor, Float4 detailColor, float strength) noexcept {
    const float amount = saturate(strength);
    const Float4 modulation{
        1.0F + (detailColor.x * 2.0F - 1.0F) * amount,
        1.0F + (detailColor.y * 2.0F - 1.0F) * amount,
        1.0F + (detailColor.z * 2.0F - 1.0F) * amount,
        1.0F,
    };
    return {baseColor.x * modulation.x, baseColor.y * modulation.y,
            baseColor.z * modulation.z, baseColor.w};
}

float blend_detail_roughness(float baseRoughness, float detailRoughness,
                             float strength) noexcept {
    const float signedDetail = detailRoughness * 2.0F - 1.0F;
    return saturate(baseRoughness + signedDetail * saturate(strength));
}

} // namespace dve
