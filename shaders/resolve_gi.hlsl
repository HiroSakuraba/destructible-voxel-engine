// Resolves one-bounce rays to incident diffuse radiance. A second, one-ray-per-bounce trace
// supplies explicit sun visibility, so sun-lit surfaces transfer colour instead of contributing
// only the hemisphere sky term.

#include "common/brick_trace_common.hlsli"
#include "common/material_table.hlsli"
#include "common/render_environment.hlsli"

static const uint kMaximumGiSamples = 16u;
static const float kPi = 3.14159265358979323846F;

StructuredBuffer<TraceResult> gGiResults : register(t6);
StructuredBuffer<TraceRay> gGiRays : register(t7);
StructuredBuffer<TraceResult> gGiSunResults : register(t9);
RWStructuredBuffer<float4> gIndirectDiffuse : register(u3);

cbuffer GiResolveConstants : register(b1) {
    uint gGiPixelCount;
    uint gGiResolvePad0;
    uint gGiResolvePad1;
    uint gGiResolvePad2;
}

float3 EnvironmentRadiance(float3 direction) {
    float upness = 0.5F + 0.5F * normalize(direction).y;
    return lerp(gGroundColor, gSkyColor, upness);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gGiPixelCount) return;
    if (gGlobalIlluminationMode != kGlobalIlluminationVoxelOneBounce) {
        gIndirectDiffuse[pixelIndex] = 0.0F.xxxx;
        return;
    }

    uint samples = min(max(gGlobalIlluminationSamples, 1u), kMaximumGiSamples);
    float3 accumulated = 0.0F.xxx;
    [loop]
    for (uint sample = 0u; sample < samples; ++sample) {
        uint index = pixelIndex * kMaximumGiSamples + sample;
        TraceResult hit = gGiResults[index];
        if (hit.hit == 0u) {
            accumulated += EnvironmentRadiance(gGiRays[index].direction);
            continue;
        }

        MaterialRecord material = LoadMaterialRecord(hit.material);
        float3 baseColor = MaterialBaseColorRGB(material);
        float3 emissive = MaterialEmissiveRGB(material);
        float3 bounceNormal = normalize(float3(hit.normal));
        float3 localAmbient = EnvironmentRadiance(bounceNormal);
        float sunVisibility = gGiSunResults[index].hit == 0u ? 1.0F : 0.0F;
        float3 directSun = gSunColor * gSunIntensity *
            saturate(dot(bounceNormal, normalize(gSunDirection))) * sunVisibility;
        float3 diffuseIncident = localAmbient + directSun * (1.0F / kPi);
        accumulated += emissive +
            baseColor * diffuseIncident * (1.0F - saturate(material.metallic));
    }
    gIndirectDiffuse[pixelIndex] = float4(
        accumulated * (gGlobalIlluminationIntensity / float(samples)), 1.0F);
}
