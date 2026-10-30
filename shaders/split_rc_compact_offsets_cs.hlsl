// Deterministic correctness-first compact direction allocator. It assigns each
// occupied hash slot a contiguous per-level slice of 32/128/512/2048 elements.
// A future parallel prefix sum should replace this single-thread scan for large
// tables. The caller owns/clears the destination, and provides entry capacity.
// diagnostics = (entries assigned, slots rejected for capacity, malformed keys).
[[vk::binding(0,0)]] StructuredBuffer<uint> keys : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<uint> config : register(t1);
[[vk::binding(2,0)]] RWStructuredBuffer<uint> offsets : register(u0);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> diagnostics : register(u1);
[numthreads(1,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(id.x!=0)return;
    uint count,stride;keys.GetDimensions(count,stride);uint outCount;offsets.GetDimensions(outCount,stride);
    const uint limit=config[0];uint total=0,rejected=0,invalid=0;
    [loop]for(uint i=0;i<min(count,outCount);++i){
        offsets[i]=0xffffffffu;const uint stored=keys[i];if(stored==0)continue;
        const uint packed=stored-1u,level=(packed>>24)&3u,sheet=(packed>>26)&7u;
        if((packed>>29)!=0u||sheet>=6u){++invalid;continue;}
        const uint size=32u<<(2u*level);
        if(size>limit-total){++rejected;continue;}
        offsets[i]=total;total+=size;
    }
    diagnostics[0]=total;diagnostics[1]=rejected;diagnostics[2]=invalid;
}
