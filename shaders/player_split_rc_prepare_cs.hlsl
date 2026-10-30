// Expand compact surface probes into rays and interval assignments. Each
// surface samples every base bin once; a deterministic permutation stratifies
// those samples over the 4x4x4 nested angular children of that bin.
struct SurfaceInput {float4 originCutoff; uint4 slots; uint4 sampleInfo;};
struct TraceInput {float4 originCutoff; float4 directionDistance; float4 unused;};
[[vk::binding(0,0)]] StructuredBuffer<SurfaceInput> surfaces : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<float4> directions : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<float4> config : register(t2);
[[vk::binding(3,0)]] RWStructuredBuffer<TraceInput> rays : register(u0);
[[vk::binding(4,0)]] RWStructuredBuffer<uint> slots : register(u1);
[[vk::binding(5,0)]] RWStructuredBuffer<uint> intervalDirections : register(u2);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;rays.GetDimensions(count,stride);if(id.x>=count)return;
    const uint surfaceIndex=id.x/32u, directionIndex=id.x%32u;
    const SurfaceInput surface=surfaces[surfaceIndex];
    // Odd stride visits all 64 nested directions over successive screen-sample indices.
    const uint suffix=(surface.sampleInfo.x*37u+directionIndex*17u)&63u;
    uint x=directionIndex&7u,y=directionIndex>>3u;
    [unroll]for(uint level=0u;level<3u;++level){
        const uint bits=(suffix>>(4u-2u*level))&3u;
        x=x*2u+(bits&1u);y=y*2u+(bits>>1u);
    }
    const float u=(float(x)+.5F)/64.0F,v=(float(y)+.5F)/32.0F;
    const float yy=1.0F-2.0F*v,radial=sqrt(max(0.0F,1.0F-yy*yy)),phi=6.283185307179586F*u;
    TraceInput ray;ray.originCutoff=surface.originCutoff;
    ray.directionDistance=float4(radial*cos(phi),yy,radial*sin(phi),config[0].x);ray.unused=0;rays[id.x]=ray;
    [unroll]for(uint level=0;level<4u;++level){
        const uint index=id.x*4u+level;
        slots[index]=surface.slots[level];
        intervalDirections[index]=(directionIndex<<(2u*level)) | (level==0u ? 0u : suffix>>(6u-2u*level));
    }
}
