#include "common/material_record.hlsli"
#include "common/material_mapping.hlsli"
#include "common/environment_ibl.hlsli"
#include "common/spherical_harmonic_exponentials.hlsli"
#include "common/cascaded_shadow_map.hlsli"
#include "common/local_light_probes.hlsli"

#include "common/live_bindings.hlsli"
#include "common/material_surface_detail.hlsli"
#include "common/live_material_sampling.hlsli"
DVE_LIVE_BIND(14,3) StructuredBuffer<uint> gLayerData : register(t37);
#include "common/layered_surface.hlsli"
DVE_LIVE_IMAGE(0,2) TextureCube<float4> gDiffuseIrradiance : register(t18);
DVE_LIVE_IMAGE(1,2) TextureCube<float4> gSpecularPrefilter : register(t19);
DVE_LIVE_IMAGE(2,2) Texture2D<float2> gEnvironmentBrdfLut : register(t20);
// The environment bind group always supplies 56 uint32s. Word 52 is a
// validity marker; 0 means no fitted probe and selects the legacy split-sum.
DVE_LIVE_BIND(3,2) StructuredBuffer<uint> gSheSpecularCoefficients : register(t43);
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
// Screen-space ambient occlusion (.r, 1 = open). Frame set binding 1; the live renderer binds a
// 1x1 white fallback when the frame has none, so the factor is then exactly 1.
DVE_LIVE_IMAGE(1,0) Texture2D<float4> gAmbientOcclusion : register(t41);
DVE_LIVE_IMAGE(1,0) SamplerState gAmbientOcclusionSampler : register(s13);
// Traced indirect diffuse light (frame set binding 2): .rgb radiance, .a 1 = use instead of the
// environment's diffuse integral. The 1x1 fallback has .a = 0.
DVE_LIVE_IMAGE(2,0) Texture2D<float4> gIndirectLight : register(t42);
DVE_LIVE_IMAGE(2,0) SamplerState gIndirectLightSampler : register(s14);
DVE_LIVE_BIND(0,0) cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection;float4 gEnvironmentParameters;float4 gCameraWorldPosition;
    // Direct sunlight and cascaded shadows (GpuLiveSunConstants). All zero = IBL only, the
    // behaviour before v2.36: renderers that never write these bytes get zeros.
    float4 gSunDirectionIntensity;      // xyz unit vector towards the sun; w radiance scale
    float4 gSunColorShadowStrength;     // rgb sun colour; w shadow strength 0..1
    float4 gCameraForwardCascadeCount;  // xyz camera forward; w cascade count (0 = unshadowed)
    float4 gCascadeSplitFar;            // view-depth far bound of each cascade, metres
    float4 gShadowAtlasTexelBias;       // xy atlas texel size; z receiver bias m; w normal bias m
    float4 gCascadeCenterRadius[4];
    float4 gCascadeRight[4];
    float4 gCascadeUp[4];
    float4 gCascadeForward[4];
    float4 gCascadeDepth[4];            // x minimum, y maximum absolute light depth
    float4 gCascadeAtlasScaleOffset[4]; // xy tile scale, zw tile offset in atlas UV
};

// Percentage-closer sample of one cascade tile, clamped to the tile interior (the atlas has no
// gutters). 1 = lit. Static and dynamic layers are both depth atlases of the same layout.
float LiveShadowTile(Texture2D<float> atlas, SamplerComparisonState comparison, float2 uv,
                     float depth, float4 scaleOffset, float2 texel) {
    const float2 tileMinimum = scaleOffset.zw + 0.5F * texel;
    const float2 tileMaximum = scaleOffset.zw + scaleOffset.xy - 0.5F * texel;
    float sum = 0.0F;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += atlas.SampleCmpLevelZero(comparison, clamp(uv + float2(x, y) * texel, tileMinimum, tileMaximum), depth);
    return sum / 9.0F;
}

float LiveSunVisibility(float3 worldPosition, float3 geometricNormal) {
    const uint cascadeCount = min((uint)gCameraForwardCascadeCount.w, 4u);
    if (cascadeCount == 0u) return 1.0F;
    const float viewDepth = dot(worldPosition - gCameraWorldPosition.xyz, gCameraForwardCascadeCount.xyz);
    if (viewDepth < 0.0F || viewDepth > gCascadeSplitFar[cascadeCount - 1u]) return 1.0F;
    const uint c = min(SelectShadowCascade(viewDepth, gCascadeSplitFar), cascadeCount - 1u);
    const float radius = max(gCascadeCenterRadius[c].w, 1.0e-5F);
    // Normal offset scales with the cascade's texel footprint so distant cascades do not acne.
    const float texelMeters = 2.0F * radius * gShadowAtlasTexelBias.x / max(gCascadeAtlasScaleOffset[c].x, 1.0e-6F);
    const float3 p = worldPosition + geometricNormal * (gShadowAtlasTexelBias.w + texelMeters);
    const float3 relative = p - gCascadeCenterRadius[c].xyz;
    const float2 tileUv = float2(dot(relative, gCascadeRight[c].xyz), -dot(relative, gCascadeUp[c].xyz)) / radius * 0.5F + 0.5F;
    if (any(tileUv < 0.0F) || any(tileUv > 1.0F)) return 1.0F;
    const float range = max(gCascadeDepth[c].y - gCascadeDepth[c].x, 1.0e-5F);
    const float depth = (dot(p, gCascadeForward[c].xyz) - gCascadeDepth[c].x - gShadowAtlasTexelBias.z) / range;
    if (depth > 1.0F) return 1.0F;
    const float2 atlasUv = tileUv * gCascadeAtlasScaleOffset[c].xy + gCascadeAtlasScaleOffset[c].zw;
    const float2 texel = gShadowAtlasTexelBias.xy;
    const float lit = min(
        LiveShadowTile(gCascadedStaticShadowDepth, gCascadedShadowSampler, atlasUv, depth, gCascadeAtlasScaleOffset[c], texel),
        LiveShadowTile(gCascadedDynamicShadowDepth, gDynamicShadowSampler, atlasUv, depth, gCascadeAtlasScaleOffset[c], texel));
    return lerp(1.0F, lit, saturate(gSunColorShadowStrength.w));
}
DVE_LIVE_BIND(0,1) cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;uint4 gObjectIdentityAndFlags;float4 gObjectMaterialParameters;
};

// The SHE path does not fetch or evaluate the legacy prefiltered specular
// cubemap. It shares the exact diffuse, Fresnel, AO and GI policy with the
// split-sum evaluator and switches only the environment specular integral.
float3 EvaluateLiveImageBasedLighting(float3 normal,float3 viewDirection,
                                      float3 baseColor,float metallic,
                                      float roughness,float specular,
                                      float ambientOcclusion,float4 indirectDiffuse,
                                      float3 worldPosition) {
    // Preserve the unmodified split-sum fast path for ordinary scenes.
    // Missing scene probes must not cost an extra SH9 evaluation or an
    // extra environment specular cubemap lookup on GTX 1080.
    if(gLocalProbeData[0].w<.5F && gLocalProbeData[3].x<.5F &&
       gSheSpecularCoefficients[52u]!=0x53484531u)
        return EvaluateImageBasedLightingWithIndirect(
            gDiffuseIrradiance,gSpecularPrefilter,gEnvironmentBrdfLut,
            gEnvironmentSampler,gSpecularSampler,gBrdfSampler,
            normal,viewDirection,baseColor,metallic,roughness,specular,
            ambientOcclusion,gEnvironmentParameters.w,gEnvironmentParameters.x,
            indirectDiffuse);
    const float3 N=normalize(normal);
    const float3 V=normalize(viewDirection);
    const float3 R=reflect(-V,N);
    const float ndv=saturate(dot(N,V));
    const float3 dielectricF0=(0.08F*saturate(specular)).xxx;
    const float3 f0=lerp(dielectricF0,max(baseColor,0.0F.xxx),saturate(metallic));
    const float2 brdf=gEnvironmentBrdfLut.SampleLevel(
        gBrdfSampler,float2(ndv,saturate(roughness)),0.0F);
    const float3 specularAlbedo=f0*brdf.x+brdf.y*saturate(50.0F*f0.g);
    const float ao=saturate(ambientOcclusion);
    const float occlusion=saturate(
        pow(ndv+ao,exp2(-16.0F*saturate(roughness)-1.0F))-1.0F+ao);

    float diffuseCoverage;
    const float3 probeIrradiance=LocalSH9Irradiance(
        worldPosition,N,diffuseCoverage);
    const float3 skyIrradiance=gDiffuseIrradiance.SampleLevel(
        gEnvironmentSampler,RotateEnvironmentDirectionY(N,gEnvironmentParameters.x),0.0F).rgb;
    // This is only a correction to the sky fallback. Traced diffuse owns
    // the pixel when alpha = 1; never add baked light on top of traced GI.
    const float3 diffuseAdjustment=
        (probeIrradiance-skyIrradiance*diffuseCoverage)*
        max(baseColor,0.0F.xxx)*((1.0F-saturate(metallic))/kPi)*
        saturate(1.0F.xxx-specularAlbedo)*ao*(1.0F-saturate(indirectDiffuse.a));

    float specularCoverage;
    const float3 localSpec=LocalSpecularRadiance(
        worldPosition,R,roughness,specularCoverage);
    if(gSheSpecularCoefficients[52u]==0x53484531u &&
       specularCoverage<=1.0e-5F &&
       saturate(roughness)*saturate(roughness)>=DVE_SHE_MIN_ALPHA &&
       saturate(roughness)*saturate(roughness)<=1.0F && ndv>=1.0e-4F) {
        float3 irradiance=ResolveDiffuseIndirectIrradiance(
            skyIrradiance,indirectDiffuse);
        float3 diffuse=irradiance*max(baseColor,0.0F.xxx)*
                       ((1.0F-saturate(metallic))/kPi);
        diffuse*=saturate(1.0F.xxx-specularAlbedo);
        const float3 envN=RotateEnvironmentDirectionY(N,gEnvironmentParameters.x);
        const float3 envV=RotateEnvironmentDirectionY(V,gEnvironmentParameters.x);
        const float3 sheBase=DveSheEvaluateBase(
            gSheSpecularCoefficients,0u,envN,envV,roughness);
        const float fresnelDenominator=max(brdf.x+brdf.y,1.0e-5F);
        const float3 sheSpecular=sheBase*
            (specularAlbedo/fresnelDenominator);
        return diffuse*DiffuseIndirectOcclusion(ao,indirectDiffuse.a)+
               sheSpecular*occlusion+diffuseAdjustment;
    }
    const float3 global=EvaluateImageBasedLightingWithIndirect(
        gDiffuseIrradiance,gSpecularPrefilter,gEnvironmentBrdfLut,
        gEnvironmentSampler,gSpecularSampler,gBrdfSampler,
        normal,viewDirection,baseColor,metallic,roughness,specular,
        ambientOcclusion,gEnvironmentParameters.w,gEnvironmentParameters.x,
        indirectDiffuse);
    const float3 skySpec=gSpecularPrefilter.SampleLevel(
        gSpecularSampler,RotateEnvironmentDirectionY(R,gEnvironmentParameters.x),
        saturate(roughness)*gEnvironmentParameters.w).rgb;
    const float3 specCorrection=(localSpec-skySpec*specularCoverage)*
        specularAlbedo*occlusion;
    return global+diffuseAdjustment+specCorrection;
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
    float metallic = saturate(material.metallic * metallicRoughnessSample.b);
    float roughness = saturate(material.roughness * metallicRoughnessSample.g);

    float3 shadingNormal = normalize(normal);
    if(hasNormal)shadingNormal=SampleLiveMaterialNormal(gLiveNormalTexture,gNormalSampler,mapping,uv,uv1,worldPosition,objectPosition,shadingNormal,(float3x3)gObjectToWorld);


    float alpha = saturate(color.a * material.baseColorA * baseColorSample.a * opacitySample) *
                        finiteMappingUse;

    float3 emissive = MaterialEmissiveRGB(material);
    if (hasEmissive) emissive *= emissiveSample.rgb;
    float layerCoverage;
    ApplyLayeredSurface(uv,uv1,worldPosition,objectPosition,normalize(normal),worldTangent,
        (float3x3)gObjectToWorld,baseColor,metallic,roughness,emissive,alpha,shadingNormal,layerCoverage);
    ApplySurfaceDetail(gLiveSurfaceDetails[0],objectPosition,worldPosition,uv,uv1,
        (float3x3)gObjectToWorld,worldTangent,baseColor,roughness,shadingNormal);
    if(material.blendMode==kBlendModeMasked)clip(alpha-mapping.alphaCutoff);
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
    // Ambient occlusion scales the image-based diffuse light and, through the BRDF's specular
    // occlusion term, the reflections; never the direct sun. The AO image has the size of the
    // render target, so SV_Position pixels map straight to its texels.
    uint aoWidth, aoHeight;
    gAmbientOcclusion.GetDimensions(aoWidth, aoHeight);
    const float bakedOcclusion = (mapping.mappingFlags & kMaterialFlagVoxelOrmOcclusion) != 0u
        ? saturate(metallicRoughnessSample.r) : 1.0F;
    const float ambientOcclusion = bakedOcclusion * saturate(gAmbientOcclusion.SampleLevel(
        gAmbientOcclusionSampler, position.xy / float2(max(aoWidth, 1u), max(aoHeight, 1u)), 0.0F).r);
    uint giWidth, giHeight;
    gIndirectLight.GetDimensions(giWidth, giHeight);
    const float4 indirect = gIndirectLight.SampleLevel(
        gIndirectLightSampler, position.xy / float2(max(giWidth, 1u), max(giHeight, 1u)), 0.0F);
    float3 ibl=EvaluateLiveImageBasedLighting(
        shadingNormal,V,baseColor,metallic,roughness,material.specular,
        ambientOcclusion,indirect,worldPosition);
    if(material.clearCoat>0){
        float fresnel=.04F+.96F*pow(1-saturate(dot(shadingNormal,V)),5);
        float3 coat=EvaluateLiveImageBasedLighting(
            shadingNormal,V,0.0F.xxx,0.0F,material.clearCoatRoughness,
            0.5F,1.0F,float4(0.0F,0.0F,0.0F,0.0F),worldPosition);
        ibl=ibl*(1-saturate(material.clearCoat)*fresnel)+coat*saturate(material.clearCoat);
    }
    ibl*=max(0,gEnvironmentParameters.y);
    // Directional shadow maps attenuate direct sunlight only, never the environment integral.
    float3 sun = 0.0F.xxx;
    if (gSunDirectionIntensity.w > 0.0F) {
        const float3 L = normalize(gSunDirectionIntensity.xyz);
        const float3 geometricNormal = normalize(normal);
        // Back-facing geometry (culling is off) shades with the side facing the camera.
        const float3 facing = dot(geometricNormal, V) < 0.0F ? -geometricNormal : geometricNormal;
        const float3 N = dot(shadingNormal, facing) < 0.0F ? -shadingNormal : shadingNormal;
        if (dot(facing, L) > 0.0F) {
            const float visibility = LiveSunVisibility(worldPosition, facing);
            sun = ShadeDirectLight(N, V, L, gSunColorShadowStrength.rgb * gSunDirectionIntensity.w * visibility,
                                   baseColor, metallic, roughness, material.specular);
        }
    }

    return float4((ibl + sun + emissive) * exp2(clamp(gEnvironmentParameters.z,-32.0F,32.0F)) * finiteMappingUse, alpha);
}
