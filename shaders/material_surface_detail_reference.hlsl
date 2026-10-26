#include "common/live_bindings.hlsli"
#include "common/material_surface_detail.hlsli"
DVE_LIVE_BIND(0,0) StructuredBuffer<SurfaceDetailRecord> gSurfaceRecords : register(t34);
DVE_LIVE_BIND(1,0) StructuredBuffer<float4> gSurfacePositions : register(t35);
DVE_LIVE_BIND(2,0) RWStructuredBuffer<float4> gSurfaceResults : register(u15);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;gSurfaceRecords.GetDimensions(count,stride);if(id.x>=count)return;
    float4 p=gSurfacePositions[id.x];gSurfaceResults[id.x]=SampleSurfacePattern(gSurfaceRecords[id.x],p.xyz,p.w);
}
