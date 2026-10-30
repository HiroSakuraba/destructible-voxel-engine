// Enumerate every allocated probe-direction pair in compact-atlas order.
// Offsets must be produced by split_rc_compact_offsets_cs and be non-overlapping.
// The caller sizes queries to the allocator's assigned entry count.
struct Query {uint slot;uint direction;};
[[vk::binding(0,0)]] StructuredBuffer<uint> keys : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<uint> offsets : register(t1);
[[vk::binding(2,0)]] RWStructuredBuffer<Query> queries : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint keyCount,stride;keys.GetDimensions(keyCount,stride);uint offsetCount,outCount;
    offsets.GetDimensions(offsetCount,stride);queries.GetDimensions(outCount,stride);
    if(id.x>=keyCount||id.x>=offsetCount)return;
    const uint stored=keys[id.x],offset=offsets[id.x];if(stored==0u||offset==0xffffffffu)return;
    const uint packed=stored-1u,level=(packed>>24)&3u,sheet=(packed>>26)&7u;
    if((packed>>29)!=0u||sheet>=6u)return;
    const uint n=32u<<(2u*level);
    if(offset>=outCount || n>outCount-offset)return;
    [loop]for(uint direction=0;direction<n;++direction){Query q;q.slot=id.x;q.direction=direction;queries[offset+direction]=q;}
}
