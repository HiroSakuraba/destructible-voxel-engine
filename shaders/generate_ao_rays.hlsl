// Generates gAoSamplesPerHit (1..64, RenderEnvironment::ambientOcclusionSamples) ambient-occlusion
// probe rays per primary-ray hit, into gAoRays[pixel * gAoSamplesPerHit + sample]. The tracer
// operates in voxel-index units; authored distances remain in metres and are converted through
// gMetersPerVoxel at this boundary. CPU mirror: reference_ambient_occlusion
// (dve/render/ambient_occlusion.hpp).

#include "common/brick_trace_common.hlsli"
#include "common/random_sampling.hlsli"
#include "common/render_environment.hlsli"

StructuredBuffer<TraceResult> gPrimaryResults : register(t5);
StructuredBuffer<TraceRay> gPrimaryRays : register(t8);
RWStructuredBuffer<TraceRay> gAoRays : register(u1);

cbuffer AoGenerationConstants : register(b1) {
    uint gPrimaryRayCount;
    float gAoMaxDistanceMeters;
    uint gFrameSeed;
    uint gAoSamplesPerHit;
}

static const float kMinimumOriginBiasVoxels = 1.0e-3F;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint globalIndex = dispatchThreadId.x;
    uint samplesPerHit = clamp(gAoSamplesPerHit, 1u, 64u);
    uint totalSamples = gPrimaryRayCount * samplesPerHit;
    if (globalIndex >= totalSamples) return;

    uint pixelIndex = globalIndex / samplesPerHit;
    uint sampleIndex = globalIndex % samplesPerHit;
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
        sampleIndex, samplesPerHit, PcgHash(pixelIndex ^ gFrameSeed));

    probe.origin = hitPosition + normal * originBias;
    probe.direction = CosineWeightedHemisphereSample(normal, randomPair);
    probe.maxDistance = MetersToVoxelUnits(gAoMaxDistanceMeters);
    probe.rayId = pixelIndex;
    gAoRays[globalIndex] = probe;
}
