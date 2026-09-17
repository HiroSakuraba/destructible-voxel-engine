// Generates one next-event-estimation sun-visibility ray for every GI bounce that hit geometry.
// The shared voxel tracer is dispatched against this buffer before resolve_gi.hlsl.

#include "common/brick_trace_common.hlsli"
#include "common/render_environment.hlsli"

static const uint kMaximumGiSamples = 16u;

StructuredBuffer<TraceResult> gGiResults : register(t6);
StructuredBuffer<TraceRay> gGiRays : register(t7);
RWStructuredBuffer<TraceRay> gGiSunRays : register(u4);

cbuffer GiSunGenerationConstants : register(b1) {
    uint gGiSunPixelCount;
    uint gGiSunGenerationPad0;
    uint gGiSunGenerationPad1;
    uint gGiSunGenerationPad2;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint globalIndex = dispatchThreadId.x;
    uint totalCapacity = gGiSunPixelCount * kMaximumGiSamples;
    if (globalIndex >= totalCapacity) return;

    uint sampleIndex = globalIndex % kMaximumGiSamples;
    uint activeSamples = gGlobalIlluminationMode == kGlobalIlluminationVoxelOneBounce
        ? min(max(gGlobalIlluminationSamples, 1u), kMaximumGiSamples) : 0u;
    TraceResult bounce = gGiResults[globalIndex];
    TraceRay sunRay = (TraceRay)0;
    if (sampleIndex >= activeSamples || bounce.hit == 0u) {
        gGiSunRays[globalIndex] = sunRay;
        return;
    }

    float3 bounceNormal = normalize(float3(bounce.normal));
    float originBias = max(1.0e-3F, MetersToVoxelUnits(gShadowBiasMeters));
    sunRay.origin = TraceHitPosition(gGiRays[globalIndex], bounce) + bounceNormal * originBias;
    sunRay.direction = normalize(gSunDirection);
    sunRay.maxDistance = MetersToVoxelUnits(gShadowMaxDistanceMeters);
    sunRay.rayId = globalIndex;
    gGiSunRays[globalIndex] = sunRay;
}
