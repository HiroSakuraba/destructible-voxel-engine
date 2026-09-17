// Reduces AO trace results to one distance-weighted ambient visibility value per primary pixel.

#include "common/brick_trace_common.hlsli"
#include "common/render_environment.hlsli"

static const uint kAoSamplesPerHit = 8u;

StructuredBuffer<TraceResult> gAoResults : register(t6);
RWStructuredBuffer<float> gAmbientOcclusion : register(u2);

cbuffer AoResolveConstants : register(b1) {
    uint gPixelCount;
    float gAoMaxDistanceMeters;
    uint gResolvePad0;
    uint gResolvePad1;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gPixelCount) return;

    float maximumDistanceVoxels = max(MetersToVoxelUnits(gAoMaxDistanceMeters), 1.0e-6F);
    float weightedOcclusion = 0.0F;
    [unroll]
    for (uint sample = 0u; sample < kAoSamplesPerHit; ++sample) {
        TraceResult result = gAoResults[pixelIndex * kAoSamplesPerHit + sample];
        if (result.hit == 0u) continue;
        float proximity = 1.0F - saturate(result.distance / maximumDistanceVoxels);
        weightedOcclusion += proximity * proximity;
    }
    gAmbientOcclusion[pixelIndex] =
        saturate(1.0F - weightedOcclusion / float(kAoSamplesPerHit));
}
