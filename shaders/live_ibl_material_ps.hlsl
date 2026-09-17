#include "common/material_record.hlsli"
#include "common/material_mapping.hlsli"
#include "common/environment_ibl.hlsli"
#include "common/cascaded_shadow_map.hlsli"

TextureCube<float4> gDiffuseIrradiance : register(t18);
TextureCube<float4> gSpecularPrefilter : register(t19);
Texture2D<float2> gEnvironmentBrdfLut : register(t20);
Texture2D<float> gCascadedStaticShadowDepth : register(t22);
Texture2D<float> gCascadedDynamicShadowDepth : register(t25);
StructuredBuffer<MaterialRecord> gLiveMaterialRecords : register(t26);
StructuredBuffer<PolygonMaterialMappingRecord> gLiveMaterialMappings : register(t27);
Texture2D<float4> gLiveBaseColorTexture : register(t28);
Texture2D<float4> gLiveMetallicRoughnessTexture : register(t29);
Texture2D<float4> gLiveNormalTexture : register(t30);
Texture2D<float4> gLiveEmissiveTexture : register(t31);
Texture2D<float4> gLiveOpacityTexture : register(t32);
SamplerState gEnvironmentSampler : register(s1);
SamplerComparisonState gCascadedShadowSampler : register(s2);

cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection;
    float4 gEnvironmentParameters;
};
cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;
    uint4 gObjectIdentityAndFlags;
    float4 gObjectMaterialParameters;
};

float SrgbChannelToLinear(float value) {
    const float clamped = saturate(value);
    return clamped <= 0.04045F ? clamped / 12.92F
                              : pow((clamped + 0.055F) / 1.055F, 2.4F);
}

float3 DecodeMaterialColor(float3 encoded, bool srgb) {
    if (!srgb) return encoded;
    return float3(SrgbChannelToLinear(encoded.r), SrgbChannelToLinear(encoded.g),
                  SrgbChannelToLinear(encoded.b));
}

float3 ApplyDerivativeNormalMap(float3 geometricNormal, float3 worldPosition, float2 mappedUv,
                                float3 encodedNormal, float normalScale) {
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
    const float handedness = dot(cross(N, tangent), bitangent) < 0.0F ? -1.0F : 1.0F;
    bitangent = normalize(cross(N, tangent)) * handedness;
    return normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                     N * tangentNormal.z);
}

float4 main(float4 position : SV_Position, float3 worldPosition : TEXCOORD0,
            float3 normal : TEXCOORD1, float2 uv : TEXCOORD2,
            float4 color : COLOR0) : SV_Target0 {
    const MaterialRecord material = gLiveMaterialRecords[0];
    const PolygonMaterialMappingRecord mapping = gLiveMaterialMappings[0];
    const float2 mappedUv = TransformMaterialUV(
        uv, float2(mapping.baseScaleX, mapping.baseScaleY),
        float2(mapping.baseOffsetX, mapping.baseOffsetY), mapping.baseRotationRadians);
    const float finiteMappingUse = all(isfinite(mappedUv)) ? 1.0F : 0.0F;

    const bool hasBaseColor = mapping.baseColorTexture != kNoMaterialTexture;
    const bool hasMetallicRoughness = mapping.metallicRoughnessTexture != kNoMaterialTexture;
    const bool hasNormal = mapping.normalTexture != kNoMaterialTexture;
    const bool hasEmissive = mapping.emissiveTexture != kNoMaterialTexture;
    const bool hasOpacity = mapping.opacityTexture != kNoMaterialTexture;

    const float4 baseColorSample = hasBaseColor
        ? gLiveBaseColorTexture.Sample(gEnvironmentSampler, mappedUv) : 1.0F.xxxx;
    const float4 metallicRoughnessSample = hasMetallicRoughness
        ? gLiveMetallicRoughnessTexture.Sample(gEnvironmentSampler, mappedUv) : 1.0F.xxxx;
    const float4 emissiveSample = hasEmissive
        ? gLiveEmissiveTexture.Sample(gEnvironmentSampler, mappedUv) : 1.0F.xxxx;
    const float opacitySample = hasOpacity
        ? gLiveOpacityTexture.Sample(gEnvironmentSampler, mappedUv).r : 1.0F;

    const bool baseColorSrgb =
        (mapping.mappingFlags & kMaterialFlagBaseColorSrgb) != 0U;
    const bool emissiveSrgb =
        (mapping.mappingFlags & kMaterialFlagEmissiveSrgb) != 0U;
    const float3 textureBaseColor = DecodeMaterialColor(baseColorSample.rgb, baseColorSrgb);
    const float3 baseColor = max(
        MaterialBaseColorRGB(material) * color.rgb * textureBaseColor, 0.0F.xxx);
    const float metallic = saturate(material.metallic * metallicRoughnessSample.b);
    const float roughness = saturate(material.roughness * metallicRoughnessSample.g);

    float3 shadingNormal = normalize(normal);
    if (hasNormal) {
        const float3 normalSample = gLiveNormalTexture.Sample(gEnvironmentSampler, mappedUv).xyz;
        shadingNormal = ApplyDerivativeNormalMap(
            shadingNormal, worldPosition, mappedUv, normalSample, mapping.normalScale);
    }

    const float3 V = normalize(-worldPosition);
    const float3 ibl = EvaluateImageBasedLighting(
        gDiffuseIrradiance, gSpecularPrefilter, gEnvironmentBrdfLut, gEnvironmentSampler,
        shadingNormal, V, baseColor, metallic, roughness, material.specular,
        1.0F, gEnvironmentParameters.w, gEnvironmentParameters.x);
    const float staticVisibility = gCascadedStaticShadowDepth.SampleCmpLevelZero(
        gCascadedShadowSampler, float2(0.5F, 0.5F), 1.0F);
    const float dynamicVisibility = gCascadedDynamicShadowDepth.SampleCmpLevelZero(
        gCascadedShadowSampler, float2(0.5F, 0.5F), 1.0F);
    const float visibility = staticVisibility * dynamicVisibility;

    float3 emissive = MaterialEmissiveRGB(material);
    if (hasEmissive) emissive *= DecodeMaterialColor(emissiveSample.rgb, emissiveSrgb);
    const float alpha = saturate(color.a * material.baseColorA * baseColorSample.a * opacitySample) *
                        finiteMappingUse;
    if (material.blendMode == kBlendModeMasked) clip(alpha - mapping.alphaCutoff);
    return float4((ibl * visibility + emissive) * finiteMappingUse, alpha);
}
