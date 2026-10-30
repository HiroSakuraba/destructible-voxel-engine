// Proof-of-integration for split RC: trace a batch of unit rays through DVE's
// existing scene voxel grid, and emit SplitRay records consumed by
// split_rc_intervals_cs. Optional gridInfo entries 2..5 add the same colour-sky,
// sun visibility, and diffuse voxel hit lighting as the player GI path.
// Physical-sky texture sampling and textured hit materials are not implemented.
#include "common/live_bindings.hlsli"
struct TraceInput { float4 originBaseCutoff; float4 directionMaxDistance; float4 skyRadiance; };
struct SplitRay { float4 hitRadianceDistance; float4 skyRadianceBaseCutoff; };
[[vk::binding(0,0)]] StructuredBuffer<TraceInput> rays : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<float4> gridInfo : register(t1);
#define gGridOrigin gridInfo[0]
#define gGridSize gridInfo[1]
#include "common/scene_voxel_trace.hlsli"
[[vk::binding(4,0)]] RWStructuredBuffer<SplitRay> splitRays : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count, stride; rays.GetDimensions(count,stride);
    if(id.x >= count) return;
    const TraceInput r=rays[id.x];
    GridHit hit;
    const bool found=TraceSceneGrid(r.originBaseCutoff.xyz,r.directionMaxDistance.xyz,
                                    r.directionMaxDistance.w,hit);
    SplitRay output;
    float3 hitRadiance=0;
    if(found){
        const float4 albedoMetallic=gGridPalette[(hit.cell-1u)*2u];
        hitRadiance=gGridPalette[(hit.cell-1u)*2u+1u].xyz;
        uint infoCount,infoStride;gridInfo.GetDimensions(infoCount,infoStride);
        if(infoCount>=6u){
            const float4 sky=gridInfo[2],ground=gridInfo[3],sun=gridInfo[4],sunColor=gridInfo[5];
            const float t=saturate(.5F+.5F*hit.normal.y);
            float3 incident=lerp(ground.xyz,sky.xyz,t);
            const float sunLength=length(sun.xyz);
            if(sun.w>0 && sunLength>1e-6F){
                const float3 sunDirection=sun.xyz/sunLength;
                const float ndotl=max(0.0F,dot(hit.normal,sunDirection));
                if(ndotl>0){
                    const float lift=max(1e-3F,1.1F*gGridOrigin.w);
                    GridHit shadow;
                    const float3 hitPoint=r.originBaseCutoff.xyz+r.directionMaxDistance.xyz*hit.t;
                    if(!TraceSceneGrid(hitPoint+hit.normal*lift,sunDirection,max(sunColor.w,0.0F),shadow))
                        incident+=sunColor.xyz*(sun.w*ndotl/3.14159265358979F);
                }
            }
            hitRadiance+=albedoMetallic.xyz*incident*(1.0F-saturate(albedoMetallic.w));
        }
    }
    output.hitRadianceDistance=found ? float4(hitRadiance,hit.t) : float4(0,0,0,-1);
    output.skyRadianceBaseCutoff=float4(r.skyRadiance.xyz,r.originBaseCutoff.w);
    splitRays[id.x]=output;
}
