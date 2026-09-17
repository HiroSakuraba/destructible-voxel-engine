// Destructible Voxel Engine v1.4 primary software tracer contract.
// Shader Model 6.0 baseline: uint32 only, no DXR, no mesh shaders, no int64.
//
// The traversal itself now lives in common/brick_trace_common.hlsli, shared with the
// ambient-occlusion, subsurface-thickness, and translucency-continuation passes added
// alongside shade_primary.hlsl; this file is just that shared trace dispatched over the
// primary ray batch, exactly as it always was before the extraction (see that header's own
// comment for what did and did not change: this is a mechanical move, not a rewrite, with the
// sole exception of two functions, NextBrickBoundary and VoxelBeforeBoundary, dropped because
// they were dead code in the original single-file version - defined, never called).

#include "common/brick_trace_common.hlsli"

StructuredBuffer<TraceRay> gRays : register(t3);
RWStructuredBuffer<TraceResult> gResults : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint rayIndex = dispatchThreadId.x;
    if (rayIndex >= gRayCount) return;
    gResults[rayIndex] = TraceVoxelRay(gRays[rayIndex]);
}
