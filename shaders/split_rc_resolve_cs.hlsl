// Resolve the 20.12 sums of split_rc_accumulate_cs. Empty or overflowed bins
// remain unavailable, rather than creating false sky or saturated radiance.
[[vk::binding(0,0)]] StructuredBuffer<uint> sums : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<float4> means : register(u0);
[[vk::binding(2,0)]] RWStructuredBuffer<uint> valid : register(u1);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count,stride;sums.GetDimensions(count,stride);
    uint outCount;means.GetDimensions(outCount,stride);
    uint validCount;valid.GetDimensions(validCount,stride);
    if(id.x>=count/6u || id.x>=outCount || id.x>=validCount) return;
    const uint base=id.x*6u, n=sums[base+4];
    if(n==0u || sums[base+5]!=0u){means[id.x]=float4(0,0,0,-1);valid[id.x]=0;return;}
    means[id.x]=float4(sums[base],sums[base+1],sums[base+2],sums[base+3])/(4096.0F*(float)n);
    valid[id.x]=1u;
}
