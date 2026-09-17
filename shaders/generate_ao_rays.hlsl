// Generates kAoSamplesPerHit ambient-occlusion probe rays per primary-ray hit. The tracer
// operates in voxel-index units; authored distances remain in metres and are converted through
// gMetersPerVoxel at this boundary.

#include "common/brick_trace_common.hlsli"
#include "common/random_sampling.hlsli"
#include "common/render_environment.hlsli"

static const uint kAoSamplesPerHit = 8u;

StructuredBuffer<TraceResult> gPrimaryResults : register(t5);
StructuredBuffer<TraceRay> gPrimaryRays : register(t8);
RWStructuredBuffer<TraceRay> gAoRays : register(u1);

cbuffer AoGenerationConstants : register(b1) {
    uint gPrimaryRayCount;
    float gAoMaxDistanceMeters;
    uint gFrameSeed;
    uint gAoConstantsPad0;
}

static const float kMinimumOriginBiasVoxels = 1.0e-3F;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint globalIndex = dispatchThreadId.x;
    uint totalSamples = gPrimaryRayCount * kAoSamplesPerHit;
    if (globalIndex >= totalSamples) return;

    uint pixelIndex = globalIndex / kAoSamplesPerHit;
    uint sampleIndex = globalIndex % kAoSamplesPerHit;
    TraceResult primary = gPrimaryResults[pixelIndex];

    TraceRay probe = (TraceRay)0;
    if (primary.hit == 0u) {
        gAoRays[globalIndex] = probe;
        return;
    }

    float3 normal = normalize(float3(primary.normal));
    float3 hitPosition = TraceHitPosition(gPrimaryRays[pixelIndex], primary);
    float originBias = max(kMinimumOriginBiasVoxels, MetersToVoxelUnits(gShadowBiasMeters));
    float2 randomPair = RotatedHammersley2D(
        sampleIndex, kAoSamplesPerHit, PcgHash(pixelIndex ^ gFrameSeed));

    probe.origin = hitPosition + normal * originBias;
    probe.direction = CosineWeightedHemisphereSample(normal, randomPair);
    probe.maxDistance = MetersToVoxelUnits(gAoMaxDistanceMeters);
    probe.rayId = pixelIndex;
    gAoRays[globalIndex] = probe;
}
