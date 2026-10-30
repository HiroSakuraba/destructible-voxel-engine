// Compose four already-resolved split RC intervals for a ray/direction, from
// farthest to nearest. This deliberately does not interpolate or average sparse
// probe neighbours; callers must supply resolved parent intervals first.
// Input w: -1 means no sample; [0,1] is interval transmittance.
// Output w: 1 means a valid merged radiance, 0 means unavailable.
[[vk::binding(0,0)]] StructuredBuffer<float4> intervals : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<float4> merged : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count,stride; intervals.GetDimensions(count,stride);
    if (id.x >= count / 4u) return;
    float4 parent=float4(0,0,0,0);
    [unroll] for(int level=3; level>=0; --level) {
        float4 v=intervals[id.x*4u+(uint)level];
        float4 next=float4(0,0,0,0);
        if(v.w>=0.0F) {
            if(v.w==0.0F) next=float4(v.xyz,1);
            else if(parent.w>0.5F) next=float4(v.xyz+v.w*parent.xyz,1);
        }
        merged[id.x*4u+(uint)level]=next;
        parent=next;
    }
}
