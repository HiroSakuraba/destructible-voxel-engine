// Real alpha transparency needs the tracer to not stop at the first hit when that hit's
// material is Translucent (see the conversation this was designed in: a single-hit tracer can
// only ever show the front-most translucent surface, not anything behind it). Rather than
// modify TraceVoxelRay's stopping condition itself (which every other caller, opaque
// visibility included, depends on staying "stop on the first real hit"), this is a thin
// wrapper: one continuation ray per layer, ignoring the material id of the layer in front of
// it, dispatched up to kMaxTranslucentLayers times by the host application (each dispatch's
// output feeds the next dispatch's input ignoreMaterial). composite_translucent.hlsl then
// blends the resulting per-pixel layer stack front-to-back.

#include "common/brick_trace_common.hlsli"

static const uint kMaxTranslucentLayers = 4u;

struct ContinuationRay {
    float3 origin;
    float maxDistance;
    float3 direction;
    uint ignoreMaterial; // the material id of the layer already accounted for in front of this ray
};

StructuredBuffer<ContinuationRay> gContinuationRays : register(t7);
RWStructuredBuffer<TraceResult> gContinuationResults : register(u3);

cbuffer ContinuationConstants : register(b1) {
    uint gContinuationRayCount;
    uint gContinuationPad0;
    uint gContinuationPad1;
    uint gContinuationPad2;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    if (index >= gContinuationRayCount) return;
    ContinuationRay continuation = gContinuationRays[index];
    TraceRay ray;
    ray.origin = continuation.origin;
    ray.maxDistance = continuation.maxDistance;
    ray.direction = continuation.direction;
    ray.rayId = index;
    gContinuationResults[index] = TraceVoxelRay(ray, continuation.ignoreMaterial);
}
