// Scatter one merged result per unique (probe slot,direction) query into the
// compact atlas consumed by the next cascade or diffuse gather. The caller must
// clear the atlas and supply unique queries; duplicate writes are a data race.
struct Query {uint slot;uint direction;};
[[vk::binding(0,0)]] StructuredBuffer<uint> keys : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<uint> offsets : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<Query> queries : register(t2);
[[vk::binding(3,0)]] StructuredBuffer<float4> values : register(t3);
[[vk::binding(4,0)]] RWStructuredBuffer<float4> atlas : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint n,stride;queries.GetDimensions(n,stride);uint valueCount;values.GetDimensions(valueCount,stride);
    if(id.x>=n||id.x>=valueCount)return;
    uint keyCount,offsetCount,atlasCount;keys.GetDimensions(keyCount,stride);offsets.GetDimensions(offsetCount,stride);atlas.GetDimensions(atlasCount,stride);
    const Query q=queries[id.x];if(q.slot>=keyCount||q.slot>=offsetCount)return;
    const uint stored=keys[q.slot];if(stored==0u)return;
    const uint packed=stored-1u,level=(packed>>24)&3u,sheet=(packed>>26)&7u;
    if((packed>>29)!=0u||sheet>=6u||q.direction>=(32u<<(2u*level)))return;
    const uint offset=offsets[q.slot];if(offset==0xffffffffu||offset>=atlasCount||q.direction>=atlasCount-offset)return;
    const float4 value=values[id.x];atlas[offset+q.direction]=(all(isfinite(value))&&value.w>0.5F&&all(value.xyz>=0))?float4(value.xyz,1):float4(0,0,0,0);
}
