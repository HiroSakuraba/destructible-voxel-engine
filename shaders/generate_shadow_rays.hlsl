// Generates bounded directional-shadow rays for hard, soft-area, contact, and hybrid modes.
// All authored distances are converted from metres to the tracer's voxel-index units.

#include "common/brick_trace_common.hlsli"
#include "common/random_sampling.hlsli"
#include "common/render_environment.hlsli"

static const uint kMaximumShadowSamples = 16u;

StructuredBuffer<TraceResult> gPrimaryResults : register(t5);
StructuredBuffer<TraceRay> gPrimaryRays : register(t8);
RWStructuredBuffer<TraceRay> gShadowRays : register(u1);

cbuffer ShadowGenerationConstants : register(b1) {
    uint gPrimaryRayCount;
    uint gShadowFrameSeed;
    uint gShadowGenerationPad0;
    uint gShadowGenerationPad1;
}

uint ActiveShadowSampleCount() {
    if (gShadowMode == kShadowOff) return 0u;
    if (gShadowMode == kShadowHard || gShadowMode == kShadowContact) return 1u;
    if (gShadowMode == kShadowHybrid) return min(max(gShadowSamples, 1u) + 1u, kMaximumShadowSamples);
    return min(max(gShadowSamples, 1u), kMaximumShadowSamples);
}

float3 SampleSunDisk(uint pixelIndex, uint sampleIndex, uint sampleCount) {
    float2 randomPair = RotatedHammersley2D(
        sampleIndex, sampleCount, PcgHash(pixelIndex ^ gShadowFrameSeed));
    float radius = sqrt(randomPair.x) * tan(gShadowSoftnessRadians);
    float angle = 6.283185307179586F * randomPair.y;
    float3 tangent, bitangent;
    BuildOrthonormalBasis(normalize(gSunDirection), tangent, bitangent);
    return normalize(gSunDirection + tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint globalIndex = dispatchThreadId.x;
    uint totalCapacity = gPrimaryRayCount * kMaximumShadowSamples;
    if (globalIndex >= totalCapacity) return;

    uint pixelIndex = globalIndex / kMaximumShadowSamples;
    uint sampleIndex = globalIndex % kMaximumShadowSamples;
    TraceResult primary = gPrimaryResults[pixelIndex];
    TraceRay ray = (TraceRay)0;
    uint activeSamples = ActiveShadowSampleCount();
    if (primary.hit == 0u || sampleIndex >= activeSamples) {
        gShadowRays[globalIndex] = ray;
        return;
    }

    float3 normal = normalize(float3(primary.normal));
    float originBias = max(1.0e-3F, MetersToVoxelUnits(gShadowBiasMeters));
    ray.origin = TraceHitPosition(gPrimaryRays[pixelIndex], primary) + normal * originBias;
    bool contactSample = gShadowMode == kShadowContact ||
        (gShadowMode == kShadowHybrid && sampleIndex == 0u);
    if (contactSample || gShadowMode == kShadowHard) {
        ray.direction = normalize(gSunDirection);
    } else {
        uint diskIndex = gShadowMode == kShadowHybrid ? sampleIndex - 1u : sampleIndex;
        uint diskSamples = gShadowMode == kShadowHybrid ? activeSamples - 1u : activeSamples;
        ray.direction = SampleSunDisk(pixelIndex, diskIndex, diskSamples);
    }
    float distanceMeters = contactSample ? gContactShadowDistanceMeters : gShadowMaxDistanceMeters;
    ray.maxDistance = MetersToVoxelUnits(distanceMeters);
    ray.rayId = pixelIndex;
    gShadowRays[globalIndex] = ray;
}
