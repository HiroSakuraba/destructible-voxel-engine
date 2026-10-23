#include "common/material_record.hlsli"
#include "common/material_mapping.hlsli"
#include "common/environment_ibl.hlsli"
#include "common/cascaded_shadow_map.hlsli"

#include "common/live_bindings.hlsli"
#include "common/material_surface_detail.hlsli"
#include "common/live_material_sampling.hlsli"
DVE_LIVE_IMAGE(0,2) TextureCube<float4> gDiffuseIrradiance : register(t18);
DVE_LIVE_IMAGE(1,2) TextureCube<float4> gSpecularPrefilter : register(t19);
DVE_LIVE_IMAGE(2,2) Texture2D<float2> gEnvironmentBrdfLut : register(t20);
DVE_LIVE_IMAGE(11,3) Texture2D<float> gCascadedStaticShadowDepth : register(t22);
DVE_LIVE_IMAGE(12,3) Texture2D<float> gCascadedDynamicShadowDepth : register(t25);
DVE_LIVE_BIND(0,3) StructuredBuffer<MaterialRecord> gLiveMaterialRecords : register(t26);
DVE_LIVE_BIND(1,3) StructuredBuffer<PolygonMaterialMappingRecord> gLiveMaterialMappings : register(t27);
DVE_LIVE_BIND(13,3) StructuredBuffer<SurfaceDetailRecord> gLiveSurfaceDetails : register(t33);
DVE_LIVE_IMAGE(2,3) Texture2D<float4> gLiveBaseColorTexture : register(t28);
DVE_LIVE_IMAGE(3,3) Texture2D<float4> gLiveMetallicRoughnessTexture : register(t29);
DVE_LIVE_IMAGE(4,3) Texture2D<float4> gLiveNormalTexture : register(t30);
DVE_LIVE_IMAGE(5,3) Texture2D<float4> gLiveEmissiveTexture : register(t31);
DVE_LIVE_IMAGE(6,3) Texture2D<float4> gLiveOpacityTexture : register(t32);
DVE_LIVE_IMAGE(0,2) SamplerState gEnvironmentSampler : register(s1);
DVE_LIVE_IMAGE(1,2) SamplerState gSpecularSampler : register(s5);
DVE_LIVE_IMAGE(2,2) SamplerState gBrdfSampler : register(s6);
DVE_LIVE_IMAGE(11,3) SamplerComparisonState gCascadedShadowSampler : register(s2);
DVE_LIVE_IMAGE(12,3) SamplerComparisonState gDynamicShadowSampler : register(s7);
DVE_LIVE_IMAGE(2,3) SamplerState gBaseColorSampler : register(s8);
DVE_LIVE_IMAGE(3,3) SamplerState gMetallicRoughnessSampler : register(s9);
DVE_LIVE_IMAGE(4,3) SamplerState gNormalSampler : register(s10);
DVE_LIVE_IMAGE(5,3) SamplerState gEmissiveSampler : register(s11);
DVE_LIVE_IMAGE(6,3) SamplerState gOpacitySampler : register(s12);
DVE_LIVE_BIND(0,0) cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection;float4 gEnvironmentParameters;float4 gCameraWorldPosition;
};
DVE_LIVE_BIND(0,1) cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;uint4 gObjectIdentityAndFlags;float4 gObjectMaterialParameters;
};

float4 main(float4 position : SV_Position, float3 worldPosition : TEXCOORD0,
            float3 normal : TEXCOORD1, float2 uv : TEXCOORD2,
            float4 color : COLOR0, float3 objectPosition : TEXCOORD3,
            float2 uv1 : TEXCOORD4, float4 worldTangent : TEXCOORD5) : SV_Target0 {
    const MaterialRecord material = gLiveMaterialRecords[0];
    const PolygonMaterialMappingRecord mapping = gLiveMaterialMappings[0];
    const float2 mappedUv = TransformMaterialUV(
        (mapping.mappingMode==kMaterialMappingUV1||mapping.baseTexcoord==1)?uv1:uv, float2(mapping.baseScaleX, mapping.baseScaleY),
        float2(mapping.baseOffsetX, mapping.baseOffsetY), mapping.baseRotationRadians);
    const float finiteMappingUse = all(isfinite(mappedUv)) ? 1.0F : 0.0F;

    const bool hasBaseColor = mapping.baseColorTexture != kNoMaterialTexture;
    const bool hasMetallicRoughness = mapping.metallicRoughnessTexture != kNoMaterialTexture;
    const bool hasNormal = mapping.normalTexture != kNoMaterialTexture;
    const bool hasEmissive = mapping.emissiveTexture != kNoMaterialTexture;
    const bool hasOpacity = mapping.opacityTexture != kNoMaterialTexture;

    const float4 baseColorSample = hasBaseColor
        ? SampleLiveMaterialMap(gLiveBaseColorTexture,gBaseColorSampler,mapping,uv,uv1,mapping.baseTexcoord,worldPosition,objectPosition,normal,(float3x3)gObjectToWorld) : 1.0F.xxxx;
    const float4 metallicRoughnessSample = hasMetallicRoughness
        ? SampleLiveMaterialMap(gLiveMetallicRoughnessTexture,gMetallicRoughnessSampler,mapping,uv,uv1,mapping.additionalTexcoords & 1U,worldPosition,objectPosition,normal,(float3x3)gObjectToWorld) : 1.0F.xxxx;
    const float4 emissiveSample = hasEmissive
        ? SampleLiveMaterialMap(gLiveEmissiveTexture,gEmissiveSampler,mapping,uv,uv1,(mapping.additionalTexcoords >> 1U) & 1U,worldPosition,objectPosition,normal,(float3x3)gObjectToWorld) : 1.0F.xxxx;
    const float opacitySample = hasOpacity
        ? SampleLiveMaterialMap(gLiveOpacityTexture,gOpacitySampler,mapping,uv,uv1,(mapping.additionalTexcoords >> 2U) & 1U,worldPosition,objectPosition,normal,(float3x3)gObjectToWorld).r : 1.0F;

    // Color residency uses sRGB image formats: decode happens before GPU filtering.
    const float3 textureBaseColor = baseColorSample.rgb;
    float3 baseColor = max(
        MaterialBaseColorRGB(material) * color.rgb * textureBaseColor, 0.0F.xxx);
    const float metallic = saturate(material.metallic * metallicRoughnessSample.b);
    float roughness = saturate(material.roughness * metallicRoughnessSample.g);

    float3 shadingNormal = normalize(normal);
    if(hasNormal)shadingNormal=SampleLiveMaterialNormal(gLiveNormalTexture,gNormalSampler,mapping,uv,uv1,worldPosition,objectPosition,shadingNormal,(float3x3)gObjectToWorld);

    ApplySurfaceDetail(gLiveSurfaceDetails[0],objectPosition,worldPosition,uv,uv1,
        (float3x3)gObjectToWorld,worldTangent,baseColor,roughness,shadingNormal);
    const float alpha = saturate(color.a * material.baseColorA * baseColorSample.a * opacitySample) *
                        finiteMappingUse;
    if (material.blendMode == kBlendModeMasked) clip(alpha - mapping.alphaCutoff);
    float3 emissive = MaterialEmissiveRGB(material);
    if (hasEmissive) emissive *= emissiveSample.rgb;
#ifdef DVE_SURFACE_DEBUG_EMISSION
    return float4(emissive,alpha);
#endif
#ifdef DVE_SURFACE_DEBUG_CHANNELS
    return float4(metallic,roughness,alpha,1);
#endif
#ifdef DVE_SURFACE_DEBUG_COLOR
    return float4(baseColor,roughness);
#endif
#ifdef DVE_SURFACE_DEBUG_NORMAL
    return float4(shadingNormal*.5F+.5F,roughness);
#endif
    const float3 V = normalize(gCameraWorldPosition.xyz-worldPosition);
    float3 ibl = EvaluateImageBasedLightingSeparateSamplers(
        gDiffuseIrradiance, gSpecularPrefilter, gEnvironmentBrdfLut, gEnvironmentSampler,gSpecularSampler,gBrdfSampler,
        shadingNormal, V, baseColor, metallic, roughness, material.specular,
        1.0F, gEnvironmentParameters.w, gEnvironmentParameters.x);
    if(material.clearCoat>0){
        float fresnel=.04F+.96F*pow(1-saturate(dot(shadingNormal,V)),5);
        float3 coat=EvaluateImageBasedLightingSeparateSamplers(gDiffuseIrradiance,gSpecularPrefilter,
            gEnvironmentBrdfLut,gEnvironmentSampler,gSpecularSampler,gBrdfSampler,shadingNormal,V,
            0.0F.xxx,0,material.clearCoatRoughness,.5F,1,gEnvironmentParameters.w,gEnvironmentParameters.x);
        ibl=ibl*(1-saturate(material.clearCoat)*fresnel)+coat*saturate(material.clearCoat);
    }
    ibl*=max(0,gEnvironmentParameters.y);
    // Directional shadow maps attenuate direct sunlight, not the environment integral.
    // This pass currently evaluates IBL; applying one fixed CSM texel blackened the
    // material whenever the atlas was uninitialized or that unrelated texel was shaded.

    return float4((ibl + emissive) * exp2(clamp(gEnvironmentParameters.z,-32.0F,32.0F)) * finiteMappingUse, alpha);
}
