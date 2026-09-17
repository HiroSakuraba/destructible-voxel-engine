// Generates stratified cosine-weighted diffuse rays for bounded one-bounce voxel GI.

#include "common/brick_trace_common.hlsli"
#include "common/random_sampling.hlsli"
#include "common/render_environment.hlsli"

static const uint kMaximumGiSamples = 16u;

StructuredBuffer<TraceResult> gPrimaryResults : register(t5);
StructuredBuffer<TraceRay> gPrimaryRays : register(t8);
RWStructuredBuffer<TraceRay> gGiRays : register(u1);

cbuffer GiGenerationConstants : register(b1) {
    uint gGiPrimaryRayCount;
    uint gGiFrameSeed;
    uint gGiGenerationPad0;
    uint gGiGenerationPad1;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint globalIndex = dispatchThreadId.x;
    uint totalCapacity = gGiPrimaryRayCount * kMaximumGiSamples;
    if (globalIndex >= totalCapacity) return;

    uint pixelIndex = globalIndex / kMaximumGiSamples;
    uint sampleIndex = globalIndex % kMaximumGiSamples;
    TraceResult primary = gPrimaryResults[pixelIndex];
    TraceRay ray = (TraceRay)0;
    uint activeSamples = gGlobalIlluminationMode == kGlobalIlluminationVoxelOneBounce
        ? min(max(gGlobalIlluminationSamples, 1u), kMaximumGiSamples) : 0u;
    if (primary.hit == 0u || sampleIndex >= activeSamples) {
        gGiRays[globalIndex] = ray;
        return;
    }

    float3 normal = normalize(float3(primary.normal));
    float2 randomPair = RotatedHammersley2D(
        sampleIndex, activeSamples, PcgHash(pixelIndex ^ gGiFrameSeed));
    float originBias = max(1.0e-3F, MetersToVoxelUnits(gShadowBiasMeters));

    ray.origin = TraceHitPosition(gPrimaryRays[pixelIndex], primary) + normal * originBias;
    ray.direction = CosineWeightedHemisphereSample(normal, randomPair);
    ray.maxDistance = MetersToVoxelUnits(gGlobalIlluminationMaxDistanceMeters);
    ray.rayId = pixelIndex;
    gGiRays[globalIndex] = ray;
}
