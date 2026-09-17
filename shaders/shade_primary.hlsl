// One shading evaluation per traced voxel material. Layer stacks are flattened by the CPU
// material resolver before upload; Material Parameter Collection values remain global and are
// read here once per pixel rather than copied into every material instance.

#include "common/brick_trace_common.hlsli"
#include "common/material_parameter_collection.hlsli"
#include "common/pbr_lighting.hlsli"
#include "common/environment_ibl.hlsli"
#include "common/render_environment.hlsli"
#include "common/subsurface.hlsli"

StructuredBuffer<TraceResult> gPrimaryResults : register(t5);
StructuredBuffer<TraceRay> gPrimaryRays : register(t8);
StructuredBuffer<float> gAmbientOcclusion : register(t9);
StructuredBuffer<float> gShadowVisibility : register(t10);
StructuredBuffer<float4> gIndirectDiffuse : register(t11);
RWStructuredBuffer<float4> gRadiance : register(u4);
TextureCube<float4> gDiffuseIrradiance : register(t18);
TextureCube<float4> gSpecularPrefilter : register(t19);
Texture2D<float2> gEnvironmentBrdfLut : register(t20);
SamplerState gEnvironmentSampler : register(s1);

cbuffer EnvironmentLightingConstants : register(b8) {
    uint gEnvironmentLightingEnabled;
    float gEnvironmentMaximumSpecularLod;
    float gEnvironmentIntensity;
    float gEnvironmentRotationRadians;
}

float3 ShadeMissPixel(float3 rayDirection) {
    float upness = 0.5F + 0.5F * normalize(rayDirection).y;
    return lerp(gGroundColor, gSkyColor, upness);
}

float3 ApplyFoliageWindNormal(float3 normal, float3 worldPosition) {
    float strength = saturate(MaterialGlobalScalar(kMpcWindStrength));
    float3 wind = MaterialGlobalVector(kMpcWindDirection).xyz;
    float lengthSquared = dot(wind, wind);
    if (strength <= 0.0F || lengthSquared <= 1.0e-8F) return normal;
    wind *= rsqrt(lengthSquared);
    float phase = dot(worldPosition, wind) * 0.31F + MaterialGlobalScalar(kMpcTimeSeconds) * 1.7F;
    // Shading-normal motion only. Geometry/occlusion stays authoritative to the voxel trace.
    return normalize(normal + wind * (sin(phase) * 0.09F * strength));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gImageWidth * gImageHeight) return;

    TraceResult hit = gPrimaryResults[pixelIndex];
    if (hit.hit == 0u) {
        gRadiance[pixelIndex] = float4(ShadeMissPixel(gPrimaryRays[pixelIndex].direction), 0.0F);
        return;
    }

    MaterialRecord material = LoadMaterialRecord(hit.material);
    float3 timeOfDayTint = max(MaterialGlobalVector(kMpcTimeOfDayTint).rgb, 0.0F.xxx);
    float wetness = saturate(MaterialGlobalScalar(kMpcWetness));
    float3 baseColor = MaterialBaseColorRGB(material) * timeOfDayTint;
    baseColor *= lerp(1.0F.xxx, 0.76F.xxx, wetness);
    float roughness = lerp(material.roughness, 0.08F, wetness);
    float3 emissive = MaterialEmissiveRGB(material);

    if (material.shadingModel == kShadingModelUnlit) {
        gRadiance[pixelIndex] = float4(baseColor, material.baseColorA);
        return;
    }
    if (material.shadingModel == kShadingModelEmissive) {
        gRadiance[pixelIndex] = float4(emissive, material.baseColorA);
        return;
    }

    float3 geometricNormal = normalize(float3(hit.normal));
    float3 viewDirection = normalize(-gPrimaryRays[pixelIndex].direction);
    float3 normal = material.shadingModel == kShadingModelTwoSidedFoliage
        ? ApplyFoliageWindNormal(geometricNormal, float3(hit.voxel) + 0.5F.xxx)
        : geometricNormal;
    float ao = gAmbientOcclusion[pixelIndex];
    float shadowVisibility = gShadowMode == kShadowOff ? 1.0F : saturate(gShadowVisibility[pixelIndex]);
    float3 sunRadiance = gSunColor * gSunIntensity * shadowVisibility;

    float3 direct = 0.0F.xxx;
    {
        if (material.shadingModel == kShadingModelClearCoat) {
            direct = ShadeClearCoatDirect(normal, viewDirection, gSunDirection, sunRadiance,
                                          baseColor, material.metallic, roughness, material.specular,
                                          material.clearCoat, material.clearCoatRoughness);
        } else if (material.shadingModel == kShadingModelTwoSidedFoliage) {
            direct = ShadeTwoSidedFoliageDirect(normal, viewDirection, gSunDirection, sunRadiance,
                                                baseColor, material.metallic, roughness, material.specular,
                                                MaterialFoliageColorRGB(material),
                                                material.foliageTransmittance, material.foliageWrap);
        } else {
            direct = ShadeDirectLight(normal, viewDirection, gSunDirection, sunRadiance,
                                      baseColor, material.metallic, roughness, material.specular);
        }
    }

    float3 ambientNormal = material.shadingModel == kShadingModelTwoSidedFoliage && dot(normal, viewDirection) < 0.0F
        ? -normal : normal;
    float3 indirect = 0.0F.xxx;
    if (gGlobalIlluminationMode == kGlobalIlluminationAmbientHemisphere &&
        gEnvironmentLightingEnabled == 0u) {
        indirect = ShadeAmbient(ambientNormal, gSkyColor, gGroundColor, ao) *
                   baseColor * (1.0F - material.metallic) * gGlobalIlluminationIntensity;
    } else if (gGlobalIlluminationMode == kGlobalIlluminationVoxelOneBounce) {
        indirect = gIndirectDiffuse[pixelIndex].rgb * ao * baseColor * (1.0F - material.metallic);
    }
    if (gEnvironmentLightingEnabled != 0u) {
        indirect += EvaluateImageBasedLighting(
            gDiffuseIrradiance, gSpecularPrefilter, gEnvironmentBrdfLut, gEnvironmentSampler,
            ambientNormal, viewDirection, baseColor, material.metallic, roughness,
            material.specular, ao, gEnvironmentMaximumSpecularLod,
            gEnvironmentRotationRadians) * max(gEnvironmentIntensity, 0.0F);
    }
    float3 radiance = direct + indirect + emissive;

    if (material.shadingModel == kShadingModelSubsurface && material.subsurfaceScatterDistanceMeters > 0.0F) {
        float interiorBiasVoxels = max(1.0e-3F, MetersToVoxelUnits(0.001F));
        float3 hitPoint = TraceHitPosition(gPrimaryRays[pixelIndex], hit) -
                          geometricNormal * interiorBiasVoxels;
        float maximumThicknessMeters = min(
            gSubsurfaceMaxDistanceMeters, material.subsurfaceScatterDistanceMeters * 8.0F);
        float thicknessVoxels = MarchSubsurfaceThicknessVoxels(
            hitPoint, -geometricNormal, hit.material,
            MetersToVoxelUnits(maximumThicknessMeters), 32u);
        float thicknessMeters = VoxelUnitsToMeters(thicknessVoxels);
        radiance += ShadeSubsurfaceTransmission(
            thicknessMeters, material.subsurfaceScatterDistanceMeters, MaterialSubsurfaceColorRGB(material),
            geometricNormal, gSunDirection, sunRadiance);
    }

    gRadiance[pixelIndex] = float4(radiance, material.baseColorA);
}
