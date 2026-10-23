#include "common/material_record.hlsli"
#include "common/material_mapping.hlsli"
#include "common/environment_ibl.hlsli"
#include "common/cascaded_shadow_map.hlsli"

#include "common/live_bindings.hlsli"
#include "common/material_surface_detail.hlsli"
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

float2 LiveUv(float2 uv,float2 uv1,PolygonMaterialMappingRecord m,uint channel){
    float2 selected=(m.mappingMode==kMaterialMappingUV1||channel==1)?uv1:uv;
    return TransformMaterialUV(selected,float2(m.baseScaleX,m.baseScaleY),float2(m.baseOffsetX,m.baseOffsetY),m.baseRotationRadians);
}
float4 SampleLiveMap(Texture2D<float4> textureMap,SamplerState textureSampler,PolygonMaterialMappingRecord m,
                     float2 mappedUv,float3 worldPosition,float3 objectPosition,float3 worldNormal){
    if(m.mappingMode<kMaterialMappingWorldTriplanar)return textureMap.Sample(textureSampler,mappedUv);
    bool local=m.mappingMode==kMaterialMappingObjectTriplanar;
    float3 p=local?objectPosition:worldPosition;
    float3 N=local?mul(transpose((float3x3)gObjectToWorld),worldNormal):worldNormal;
    float3 weights=MaterialTriplanarWeights(N,m.triplanarBlendSharpness);
    float2 x,y,z;MaterialTriplanarCoordinates(p,m.triplanarScale,x,y,z);
    float2 scale=float2(m.baseScaleX,m.baseScaleY),offset=float2(m.baseOffsetX,m.baseOffsetY);
    x=TransformMaterialUV(x,scale,offset,m.baseRotationRadians);y=TransformMaterialUV(y,scale,offset,m.baseRotationRadians);z=TransformMaterialUV(z,scale,offset,m.baseRotationRadians);
    // Derivatives originate in the selected position space, never an unused UV channel.
    return textureMap.SampleGrad(textureSampler,x,ddx(x),ddy(x))*weights.x+
           textureMap.SampleGrad(textureSampler,y,ddx(y),ddy(y))*weights.y+
           textureMap.SampleGrad(textureSampler,z,ddx(z),ddy(z))*weights.z;
}
float3 SampleLiveNormal(PolygonMaterialMappingRecord m,float2 mappedUv,float3 worldPosition,float3 objectPosition,float3 N){
    if(m.mappingMode<kMaterialMappingWorldTriplanar)return ApplyDerivativeNormalMap(N,worldPosition,mappedUv,gLiveNormalTexture.Sample(gNormalSampler,mappedUv).xyz,m.normalScale);
    bool local=m.mappingMode==kMaterialMappingObjectTriplanar;
    float3 p=local?objectPosition:worldPosition;
    float3 sourceNormal=local?mul(transpose((float3x3)gObjectToWorld),N):N;
    float3 weights=MaterialTriplanarWeights(sourceNormal,m.triplanarBlendSharpness);
    float2 x,y,z;MaterialTriplanarCoordinates(p,m.triplanarScale,x,y,z);
    float2 scale=float2(m.baseScaleX,m.baseScaleY),offset=float2(m.baseOffsetX,m.baseOffsetY);
    x=TransformMaterialUV(x,scale,offset,m.baseRotationRadians);y=TransformMaterialUV(y,scale,offset,m.baseRotationRadians);z=TransformMaterialUV(z,scale,offset,m.baseRotationRadians);
    float3 tx=gLiveNormalTexture.SampleGrad(gNormalSampler,x,ddx(x),ddy(x)).xyz*2-1;
    float3 ty=gLiveNormalTexture.SampleGrad(gNormalSampler,y,ddx(y),ddy(y)).xyz*2-1;
    float3 tz=gLiveNormalTexture.SampleGrad(gNormalSampler,z,ddx(z),ddy(z)).xyz*2-1;
    tx.xy*=m.normalScale;ty.xy*=m.normalScale;tz.xy*=m.normalScale;
    // Whiteout triplanar blend (matches triplanar_whiteout_normal in polygon_renderer.cpp):
    // a neutral (0,0,1) sample reproduces sourceNormal exactly on every projection.
    float3 sn=normalize(sourceNormal);
    float3 wx=float3(abs(tx.z)*sn.x,tx.y+sn.y,tx.x+sn.z);
    float3 wy=float3(ty.x+sn.x,abs(ty.z)*sn.y,ty.y+sn.z);
    float3 wz=float3(tz.x+sn.x,tz.y+sn.y,abs(tz.z)*sn.z);
    float3 normal=normalize(wx*weights.x+wy*weights.y+wz*weights.z);
    return local?normalize(mul((float3x3)gObjectToWorld,normal)):normal;
}

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
        ? SampleLiveMap(gLiveBaseColorTexture,gBaseColorSampler,mapping,mappedUv,worldPosition,objectPosition,normal) : 1.0F.xxxx;
    const float4 metallicRoughnessSample = hasMetallicRoughness
        ? SampleLiveMap(gLiveMetallicRoughnessTexture,gMetallicRoughnessSampler,mapping,mappedUv,worldPosition,objectPosition,normal) : 1.0F.xxxx;
    const float4 emissiveSample = hasEmissive
        ? SampleLiveMap(gLiveEmissiveTexture,gEmissiveSampler,mapping,mappedUv,worldPosition,objectPosition,normal) : 1.0F.xxxx;
    const float opacitySample = hasOpacity
        ? SampleLiveMap(gLiveOpacityTexture,gOpacitySampler,mapping,mappedUv,worldPosition,objectPosition,normal).r : 1.0F;

    const bool baseColorSrgb =
        (mapping.mappingFlags & kMaterialFlagBaseColorSrgb) != 0U;
    const bool emissiveSrgb =
        (mapping.mappingFlags & kMaterialFlagEmissiveSrgb) != 0U;
    const float3 textureBaseColor = DecodeMaterialColor(baseColorSample.rgb, baseColorSrgb);
    float3 baseColor = max(
        MaterialBaseColorRGB(material) * color.rgb * textureBaseColor, 0.0F.xxx);
    const float metallic = saturate(material.metallic * metallicRoughnessSample.b);
    float roughness = saturate(material.roughness * metallicRoughnessSample.g);

    float3 shadingNormal = normalize(normal);
    if(hasNormal)shadingNormal=SampleLiveNormal(mapping,LiveUv(uv,uv1,mapping,mapping.normalTexcoord),worldPosition,objectPosition,shadingNormal);

    ApplySurfaceDetail(gLiveSurfaceDetails[0],objectPosition,worldPosition,uv,uv1,
        (float3x3)gObjectToWorld,worldTangent,baseColor,roughness,shadingNormal);
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

    float3 emissive = MaterialEmissiveRGB(material);
    if (hasEmissive) emissive *= DecodeMaterialColor(emissiveSample.rgb, emissiveSrgb);
    const float alpha = saturate(color.a * material.baseColorA * baseColorSample.a * opacitySample) *
                        finiteMappingUse;
    if (material.blendMode == kBlendModeMasked) clip(alpha - mapping.alphaCutoff);
    return float4((ibl + emissive) * exp2(clamp(gEnvironmentParameters.z,-32.0F,32.0F)) * finiteMappingUse, alpha);
}
