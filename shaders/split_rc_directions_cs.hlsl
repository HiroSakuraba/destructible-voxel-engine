// Equal-area sphere directions in a hierarchical 8x4 base grid. Each direction
// has four children whose indices are 4*parent + [0,3], matching the angular
// gather convention used by split_rc_spatial_merge_cs. This is a deterministic
// direction grid, not yet the paper's per-pixel hierarchical ray allocator.
struct Request {uint level;uint direction;uint pad0;uint pad1;};
[[vk::binding(0,0)]] StructuredBuffer<Request> requests : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<float4> output : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;requests.GetDimensions(count,stride);uint n;output.GetDimensions(n,stride);
    if(id.x>=count||id.x>=n)return;
    const Request r=requests[id.x];output[id.x]=float4(0,0,0,0);
    if(r.level>3u || r.direction >= (32u << (2u*r.level)))return;
    const uint base=r.direction>>(2u*r.level);uint x=base&7u,y=base>>3;
    [loop]for(uint level=0;level<r.level;++level){
        const uint bits=(r.direction>>(2u*(r.level-level-1u)))&3u;
        x=x*2u+(bits&1u);y=y*2u+(bits>>1);
    }
    const float width=(float)(8u<<r.level),height=(float)(4u<<r.level);
    const float u=((float)x+.5F)/width,v=((float)y+.5F)/height;
    const float yy=1.0F-2.0F*v,radial=sqrt(max(0.0F,1.0F-yy*yy)),phi=6.283185307179586F*u;
    output[id.x]=float4(radial*cos(phi),yy,radial*sin(phi),1);
}
