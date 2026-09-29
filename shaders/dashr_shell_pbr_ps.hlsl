#include "common/dashr_live_trace.hlsli"
#include "common/material_record.hlsli"
#include "common/material_mapping.hlsli"
#include "common/environment_ibl.hlsli"

StructuredBuffer<MaterialRecord> gDashrMaterialRecords : register(t0, space2);
StructuredBuffer<PolygonMaterialMappingRecord> gDashrMaterialMappings : register(t1, space2);
Texture2D<float4> gDashrBaseColorTexture : register(t2, space2);
Texture2D<float4> gDashrMetallicRoughnessTexture : register(t3, space2);
Texture2D<float4> gDashrNormalTexture : register(t4, space2);
Texture2D<float4> gDashrEmissiveTexture : register(t5, space2);
Texture2D<float4> gDashrOpacityTexture : register(t6, space2);
SamplerState gDashrBaseColorSampler : register(s7, space2);
SamplerState gDashrMetallicRoughnessSampler : register(s8, space2);
SamplerState gDashrNormalSampler : register(s9, space2);
SamplerState gDashrEmissiveSampler : register(s10, space2);
SamplerState gDashrOpacitySampler : register(s11, space2);

TextureCube<float4> gDashrDiffuseIrradiance : register(t0, space3);
TextureCube<float4> gDashrSpecularPrefilter : register(t1, space3);
Texture2D<float2> gDashrEnvironmentBrdfLut : register(t2, space3);
SamplerState gDashrEnvironmentSampler : register(s3, space3);

Texture2D<float> gDashrStaticShadowDepth : register(t0, space4);
Texture2D<float> gDashrDynamicShadowDepth : register(t1, space4);
SamplerComparisonState gDashrShadowSampler : register(s2, space4);

struct DashrShellPixelInput {
    float4 position : SV_Position;
    float3 objectPosition : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct DashrShellPixelOutput {
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

float DashrSrgbChannelToLinear(float value) {
    const float clamped = saturate(value);
    return clamped <= 0.04045F ? clamped / 12.92F
                              : pow((clamped + 0.055F) / 1.055F, 2.4F);
}

float3 DashrDecodeMaterialColor(float3 encoded, bool srgb) {
    if (!srgb) return encoded;
    return float3(
        DashrSrgbChannelToLinear(encoded.r),
        DashrSrgbChannelToLinear(encoded.g),
        DashrSrgbChannelToLinear(encoded.b));
}

float3 DashrApplyDerivativeNormalMap(
    float3 geometricNormal,
    float3 worldPosition,
    float2 mappedUv,
    float3 encodedNormal,
    float normalScale) {
    const float3 N = normalize(geometricNormal);
    float3 tangentNormal = encodedNormal * 2.0F - 1.0F;
    tangentNormal.xy *= max(normalScale, 0.0F);
    tangentNormal = normalize(tangentNormal);

    const float3 dpdx = ddx(worldPosition);
    const float3 dpdy = ddy(worldPosition);
    const float2 duvdx = ddx(mappedUv);
    const float2 duvdy = ddy(mappedUv);
    const float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) <= 1.0e-8F) return N;

    const float inverseDeterminant = rcp(determinant);
    float3 tangent = (dpdx * duvdy.y - dpdy * duvdx.y) * inverseDeterminant;
    float3 bitangent = (dpdy * duvdx.x - dpdx * duvdy.x) * inverseDeterminant;
    tangent -= N * dot(N, tangent);
    const float tangentLengthSquared = dot(tangent, tangent);
    if (tangentLengthSquared <= 1.0e-12F) return N;
    tangent *= rsqrt(tangentLengthSquared);
    const float handedness =
        dot(cross(N, tangent), bitangent) < 0.0F ? -1.0F : 1.0F;
    bitangent = normalize(cross(N, tangent)) * handedness;
    return normalize(
        tangent * tangentNormal.x +
        bitangent * tangentNormal.y +
        N * tangentNormal.z);
}

DashrShellPixelOutput main(DashrShellPixelInput input) {
    const DashrTraceHit hit = DashrTraceSurface(input.objectPosition, input.uv);
    if (!hit.hit) discard;

    const MaterialRecord material = gDashrMaterialRecords[0];
    const PolygonMaterialMappingRecord mapping = gDashrMaterialMappings[0];

    const float2 mappedUv = TransformMaterialUV(
        hit.surfacePosition.xy,
        float2(mapping.baseScaleX, mapping.baseScaleY),
        float2(mapping.baseOffsetX, mapping.baseOffsetY),
        mapping.baseRotationRadians);
    if (!all(isfinite(mappedUv))) discard;

    const bool hasBaseColor = mapping.baseColorTexture != kNoMaterialTexture;
    const bool hasMetallicRoughness =
        mapping.metallicRoughnessTexture != kNoMaterialTexture;
    const bool hasNormal = mapping.normalTexture != kNoMaterialTexture;
    const bool hasEmissive = mapping.emissiveTexture != kNoMaterialTexture;
    const bool hasOpacity = mapping.opacityTexture != kNoMaterialTexture;

    const float4 baseColorSample = hasBaseColor
        ? gDashrBaseColorTexture.Sample(gDashrBaseColorSampler, mappedUv)
        : 1.0F.xxxx;
    const float4 metallicRoughnessSample = hasMetallicRoughness
        ? gDashrMetallicRoughnessTexture.Sample(
            gDashrMetallicRoughnessSampler, mappedUv)
        : 1.0F.xxxx;
    const float4 emissiveSample = hasEmissive
        ? gDashrEmissiveTexture.Sample(gDashrEmissiveSampler, mappedUv)
        : 1.0F.xxxx;
    const float opacitySample = hasOpacity
        ? gDashrOpacityTexture.Sample(gDashrOpacitySampler, mappedUv).r
        : 1.0F;

    const bool baseColorSrgb =
        (mapping.mappingFlags & kMaterialFlagBaseColorSrgb) != 0U;
    const bool emissiveSrgb =
        (mapping.mappingFlags & kMaterialFlagEmissiveSrgb) != 0U;

    const float3 textureBaseColor =
        DashrDecodeMaterialColor(baseColorSample.rgb, baseColorSrgb);
    const float3 baseColor =
        max(MaterialBaseColorRGB(material) * textureBaseColor, 0.0F.xxx);
    const float metallic =
        saturate(material.metallic * metallicRoughnessSample.b);
    const float roughness =
        saturate(material.roughness * metallicRoughnessSample.g);

    const float4 worldHit4 =
        mul(gObjectToWorld, float4(hit.objectPosition, 1.0F));
    if (abs(worldHit4.w) <= 1.0e-8F) discard;
    const float3 worldPosition = worldHit4.xyz / worldHit4.w;

    float3 shadingNormal =
        normalize(mul((float3x3)gObjectToWorld, hit.normalObject));
    if (hasNormal) {
        const float3 normalSample =
            gDashrNormalTexture.Sample(gDashrNormalSampler, mappedUv).xyz;
        shadingNormal = DashrApplyDerivativeNormalMap(
            shadingNormal, worldPosition, mappedUv,
            normalSample, mapping.normalScale);
    }

    const float3 viewVector = gCameraWorldAndDebug.xyz - worldPosition;
    const float viewLengthSquared = dot(viewVector, viewVector);
    const float3 V = viewLengthSquared > 1.0e-12F
        ? viewVector * rsqrt(viewLengthSquared)
        : float3(0.0F, 0.0F, 1.0F);

    float3 ibl = EvaluateImageBasedLighting(
        gDashrDiffuseIrradiance,
        gDashrSpecularPrefilter,
        gDashrEnvironmentBrdfLut,
        gDashrEnvironmentSampler,
        shadingNormal,
        V,
        baseColor,
        metallic,
        roughness,
        material.specular,
        1.0F,
        gEnvironmentParameters.w,
        gEnvironmentParameters.x);

    // Until the live CSM coordinate packet is moved into an explicit descriptor
    // contract, match the ordinary live material path's current conservative
    // placeholder visibility query.
    const float staticVisibility =
        gDashrStaticShadowDepth.SampleCmpLevelZero(
            gDashrShadowSampler, float2(0.5F,0.5F), 1.0F);
    const float dynamicVisibility =
        gDashrDynamicShadowDepth.SampleCmpLevelZero(
            gDashrShadowSampler, float2(0.5F,0.5F), 1.0F);
    const float visibility = staticVisibility * dynamicVisibility;

    float3 emissive = MaterialEmissiveRGB(material);
    if (hasEmissive) {
        emissive *= DashrDecodeMaterialColor(
            emissiveSample.rgb, emissiveSrgb);
    }

    const float alpha = saturate(
        material.baseColorA * baseColorSample.a * opacitySample);
    if (material.blendMode == kBlendModeMasked) {
        clip(alpha - mapping.alphaCutoff);
    }

    if (material.shadingModel == kShadingModelUnlit) {
        ibl = baseColor;
    } else if (material.shadingModel == kShadingModelEmissive) {
        ibl = 0.0F.xxx;
    }

    DashrShellPixelOutput output;
    output.color = float4(ibl * visibility + emissive, alpha);
    output.depth = DashrDepthFromObjectPosition(hit.objectPosition);
    return output;
}
