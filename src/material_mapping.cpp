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
    const HeightSampleFunction& sampleHeight, const ParallaxTraceOptions& options) {
    ParallaxMappingResult result{uv, 0U, true};
    if (settings.heightMode == HeightMappingMode::Off || settings.heightScale <= 0.0F ||
        cameraDistanceMeters >= settings.maximumParallaxDistanceMeters) {
        return result;
    }

    if (!validate_material_mapping_settings(settings) || options.maximumSteps==0 || options.maximumSteps>128 ||
        !finite(options.pixelFootprintUv) || options.pixelFootprintUv<0 || !finite(options.heightBounds) ||
        options.heightBounds.x<0 || options.heightBounds.y>1 || options.heightBounds.x>options.heightBounds.y || !finite(uv) ||
        !finite(cameraDistanceMeters) || cameraDistanceMeters < 0.0F || !sampleHeight ||
        !std::isfinite(tangentSpaceViewDirection.x) || !std::isfinite(tangentSpaceViewDirection.y) ||
        !std::isfinite(tangentSpaceViewDirection.z)) {
        result.valid = false;
        return result;
    }
    const Float3 view = normalize3(tangentSpaceViewDirection);
    if (view.z <= 0.0F) return result;
    const float viewZ = std::max(view.z, 0.08F);
    const float distanceFade = saturate(1.0F - cameraDistanceMeters /
        settings.maximumParallaxDistanceMeters);
    const float grazingFade = saturate(view.z / 0.08F);
    const float projectedFade=options.pixelFootprintUv>0?saturate(settings.heightScale/options.pixelFootprintUv):1.0F;
    const Float2 direction{view.x / viewZ * settings.heightScale * distanceFade * grazingFade * projectedFade,
                           view.y / viewZ * settings.heightScale * distanceFade * grazingFade * projectedFade};
    if (settings.heightMode == HeightMappingMode::OffsetParallax) {
        const float height = sanitized_height(sampleHeight, uv);
        result.uv = {uv.x - direction.x * (height - settings.heightReferencePlane),
                     uv.y - direction.y * (height - settings.heightReferencePlane)};
        result.samples = 1U;
        result.hit = true;
        result.layerDepth = height;
        return result;
    }
    // Height textures store depth below the shell top, matching the existing DVE
    // convention. At depth == referencePlane the UV must equal the original UV.
    const auto at = [&](float depth) {
        return Float2{uv.x - direction.x * (depth - settings.heightReferencePlane),
                      uv.y - direction.y * (depth - settings.heightReferencePlane)};
    };
    const std::uint32_t desired = std::min(step_count(view, settings),options.maximumSteps);
    const auto steps=std::max(1U,static_cast<std::uint32_t>(std::ceil(static_cast<float>(desired)*projectedFade)));
    float lo = options.heightBounds.x, hi = lo;
    float height = sanitized_height(sampleHeight, at(hi));
    result.samples = 1U;
    bool crossing = hi >= height;
    // Include depth 1: the old sample budget stopped one interval short and then
    // refined even when it had no bracket (e.g. a constant-one texture).
    for (std::uint32_t i = 1U; i <= steps && !crossing; ++i) {
        lo = hi;
        hi = options.heightBounds.x+(options.heightBounds.y-options.heightBounds.x)*static_cast<float>(i) / static_cast<float>(steps);
        height = sanitized_height(sampleHeight, at(hi));
        ++result.samples;
        crossing = hi >= height;
    }
    if (!crossing) { result.valid = false; return result; }
    if (settings.heightMode == HeightMappingMode::ParallaxOcclusion) {
        for (std::uint32_t i = 0; i < settings.refinementSteps && hi > lo; ++i) {
            const float mid = (lo + hi) * 0.5F;
            const float h = sanitized_height(sampleHeight, at(mid));
            ++result.samples;
            if (mid < h) lo = mid; else hi = mid;
        }
    }
    result.hit = true;
    result.layerDepth = (settings.heightMode == HeightMappingMode::SteepParallax) ? hi : (lo + hi) * 0.5F;
    result.uv = at(result.layerDepth);
    return result;
}

Float3 height_bump_normal(Float2 uv,Float2 deltaUv,float strength,const HeightSampleFunction& sampleHeight){
    if(!sampleHeight||!finite(uv)||!finite(deltaUv)||deltaUv.x<=0||deltaUv.y<=0||!finite(strength))return {0,0,1};
    const float du=(sanitized_height(sampleHeight,{uv.x+deltaUv.x,uv.y})-sanitized_height(sampleHeight,{uv.x-deltaUv.x,uv.y}))/(2*deltaUv.x);
    const float dv=(sanitized_height(sampleHeight,{uv.x,uv.y+deltaUv.y})-sanitized_height(sampleHeight,{uv.x,uv.y-deltaUv.y}))/(2*deltaUv.y);
    return normalize3({du*strength,dv*strength,1});
}

HeightShadowResult parallax_height_shadow(Float2 uv,float depth,Float3 light,const MaterialMappingSettings& settings,
    const HeightSampleFunction& sampleHeight,std::uint32_t maximumSteps){
    HeightShadowResult result;
    if(!sampleHeight||!finite(uv)||!finite(depth)||depth<0||depth>1||maximumSteps==0||maximumSteps>64||
       !finite(light.x)||!finite(light.y)||!finite(light.z)||!validate_material_mapping_settings(settings))return result;
    light=normalize3(light);
    if(light.z<=0){result.visibility=0;return result;}
    for(std::uint32_t i=1;i<=maximumSteps;++i){
        const float t=depth*static_cast<float>(i)/static_cast<float>(maximumSteps);
        const Float2 q{uv.x+light.x/std::max(light.z,0.08F)*settings.heightScale*t,
                       uv.y+light.y/std::max(light.z,0.08F)*settings.heightScale*t};
        const float h=sanitized_height(sampleHeight,q);++result.samples;
        if(depth-t>h+1e-3F){result.visibility=0;break;}
    }
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
        base.z * (1.0F + (std::max(0.0F, detail.z) - 1.0F) * amount),
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
