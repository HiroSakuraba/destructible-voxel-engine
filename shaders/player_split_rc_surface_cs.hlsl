// Experimental surface extraction from the player's depth prepass. Input set 0
// uses the existing player screen/depth bindings. The output is one sample per
// grid cell (spacing in gRcLevel.z) in row-major order. Invalid pixels have w=0.
#include "common/player_screen_geometry.hlsli"
struct Surface {float4 position;float4 normal;};
DVE_LIVE_BIND(9,0) RWStructuredBuffer<Surface> gSplitSurfaces : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;gSplitSurfaces.GetDimensions(count,stride);if(id.x>=count)return;
    Surface result;result.position=0;result.normal=0;
    const uint step=(uint)max(1.0F,gRcLevel.z);
    const uint width=(uint)gTarget.x,height=(uint)gTarget.y;
    if(width==0||height==0){gSplitSurfaces[id.x]=result;return;}
    const uint cols=(width+step-1u)/step,rows=(height+step-1u)/step;
    if(id.x>=cols*rows){gSplitSurfaces[id.x]=result;return;}
    const uint2 sample=uint2(id.x%cols,id.x/cols);
    const int2 pixel=int2(min(sample*step+step/2u,uint2(width-1u,height-1u)));
    float3 position,normal;
    if(SurfaceAtPixel(pixel,position,normal)){result.position=float4(position,1);result.normal=float4(normal,1);}
    gSplitSurfaces[id.x]=result;
}
