// Split RC player tracing uses the exact same voxel hit lighting and physical
// sky tables as the existing player GI shaders. Rays must have unit directions
// and start outside the source surface, using the player's usual grid-cell lift.
#include "common/player_screen_geometry.hlsli"
#include "common/player_gi_common.hlsli"
struct TraceInput {float4 originBaseCutoff;float4 directionMaxDistance;float4 unused;};
struct SplitRay {float4 hitRadianceDistance;float4 skyRadianceBaseCutoff;};
DVE_LIVE_BIND(0,1) StructuredBuffer<TraceInput> gSplitRays : register(t10);
DVE_LIVE_BIND(1,1) RWStructuredBuffer<SplitRay> gSplitRayResults : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint n,stride,outCount;gSplitRays.GetDimensions(n,stride);gSplitRayResults.GetDimensions(outCount,stride);
    if(id.x>=n||id.x>=outCount)return;
    const TraceInput ray=gSplitRays[id.x];
    GridHit hit;const bool found=gGridOrigin.w>0 && TraceSceneGrid(ray.originBaseCutoff.xyz,ray.directionMaxDistance.xyz,ray.directionMaxDistance.w,hit);
    SplitRay result;
    const float lift=max(max(1e-3F,gParams.z),1.1F*gGridOrigin.w);
    result.hitRadianceDistance=found?float4(BounceRadiance(ray.originBaseCutoff.xyz+ray.directionMaxDistance.xyz*hit.t,hit,lift),hit.t):float4(0,0,0,-1);
    result.skyRadianceBaseCutoff=float4(SkyBlend(ray.directionMaxDistance.xyz),ray.originBaseCutoff.w);
    gSplitRayResults[id.x]=result;
}
