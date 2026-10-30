// Camera ray fixture using the player's scene-grid DDA. Inputs are supplied by
// the host camera; the result contains world position, normal, and palette id.
#include "common/live_bindings.hlsli"
struct Ray {float4 origin;float4 direction;};
struct Surface {float4 position;float4 normalMaterial;};
[[vk::binding(0,0)]] StructuredBuffer<Ray> rays : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<float4> gridInfo : register(t1);
#define gGridOrigin gridInfo[0]
#define gGridSize gridInfo[1]
#include "common/scene_voxel_trace.hlsli"
[[vk::binding(4,0)]] RWStructuredBuffer<Surface> surfaces : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
 uint n,stride,outCount;rays.GetDimensions(n,stride);surfaces.GetDimensions(outCount,stride);if(id.x>=n||id.x>=outCount)return;
 Ray ray=rays[id.x];GridHit hit;Surface s;s.position=0;s.normalMaterial=0;
 if(TraceSceneGrid(ray.origin.xyz,ray.direction.xyz,ray.direction.w,hit)){
  s.position=float4(ray.origin.xyz+ray.direction.xyz*hit.t,1);
  s.normalMaterial=float4(hit.normal,(float)hit.cell);
 }
 surfaces[id.x]=s;
}
