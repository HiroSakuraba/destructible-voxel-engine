// Clear the per-frame accumulation and parent atlases on the device.
[[vk::binding(0,0)]] RWStructuredBuffer<uint> sums : register(u0);
[[vk::binding(1,0)]] RWStructuredBuffer<float4> atlasA : register(u1);
[[vk::binding(2,0)]] RWStructuredBuffer<float4> atlasB : register(u2);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;atlasA.GetDimensions(count,stride);if(id.x>=count)return;
    atlasA[id.x]=0;atlasB[id.x]=0;
    [unroll]for(uint i=0;i<6u;++i)sums[id.x*6u+i]=0;
}
