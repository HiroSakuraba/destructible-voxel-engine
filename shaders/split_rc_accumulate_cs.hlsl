// Experimental per-probe/direction interval accumulation. Each input interval
// has a matching (probe slot, direction) assignment. The caller zeroes sums and
// diagnostics and supplies a stable, current-frame probe table. The result is
// 20.12 unsigned fixed point, with overflow invalidating the entire bin.
// This does not allocate parent probes or assign ray directions.
[[vk::binding(0,0)]] StructuredBuffer<float4> intervals : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<uint> probeSlots : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<uint> directions : register(t2);
[[vk::binding(3,0)]] StructuredBuffer<uint4> config : register(t3); // x hash slots, y directional-bin capacity
[[vk::binding(4,0)]] RWStructuredBuffer<uint> sums : register(u0); // rgb, beta, count, overflow flag
[[vk::binding(5,0)]] RWStructuredBuffer<uint> diagnostics : register(u1); // invalid assignment, invalid interval, overflow
[[vk::binding(6,0)]] StructuredBuffer<uint> offsets : register(t4);
[[vk::binding(7,0)]] StructuredBuffer<uint> keys : register(t5);
bool AddBounded(uint index,uint value) {
    uint old; InterlockedAdd(sums[index],0u,old);
    [loop] for (;;) {
        if (value > 0xffffffffu-old) return false;
        uint prior; InterlockedCompareExchange(sums[index],old,old+value,prior);
        if (prior == old) return true;
        old = prior;
    }
}
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count,stride;intervals.GetDimensions(count,stride);
    if(id.x>=count) return;
    const float4 v=intervals[id.x];
    if(v.w==-1.0F) return; // no interval beyond this ray's terminal hit
    if(!all(isfinite(v)) || any(v.xyz<0.0F) || any(v.xyz>1048575.0F) || v.w<0 || v.w>1) {
        uint ignored; InterlockedAdd(diagnostics[1],1,ignored); return;
    }
    uint slotCount,dirCount;probeSlots.GetDimensions(slotCount,stride);directions.GetDimensions(dirCount,stride);
    if(id.x>=slotCount || id.x>=dirCount){uint ignored;InterlockedAdd(diagnostics[0],1,ignored);return;}
    const uint2 a=uint2(probeSlots[id.x],directions[id.x]); const uint4 cfg=config[0];
    uint sumCount;sums.GetDimensions(sumCount,stride);
    uint keyCount,offsetCount;keys.GetDimensions(keyCount,stride);offsets.GetDimensions(offsetCount,stride);
    if(a.x>=cfg.x || a.x>=keyCount || a.x>=offsetCount) {
        uint ignored;InterlockedAdd(diagnostics[0],1,ignored);return;
    }
    const uint stored=keys[a.x],offset=offsets[a.x];
    if(stored==0 || offset==0xffffffffu) {uint ignored;InterlockedAdd(diagnostics[0],1,ignored);return;}
    const uint packed=stored-1u,level=(packed>>24)&3u,sheet=(packed>>26)&7u;
    const uint levelDirections=32u<<(2u*level),limit=min(cfg.y,sumCount/6u);
    if((packed>>29)!=0u || sheet>=6u || a.y>=levelDirections || offset>=limit || a.y>=limit-offset) {
        uint ignored;InterlockedAdd(diagnostics[0],1,ignored);return;
    }
    const uint base=(offset+a.y)*6u;
    const uint4 q=(uint4)round(v*4096.0F);
    bool ok=true;
    ok=AddBounded(base+0,q.x)&&ok;
    ok=AddBounded(base+1,q.y)&&ok;
    ok=AddBounded(base+2,q.z)&&ok;
    ok=AddBounded(base+3,q.w)&&ok;
    ok=AddBounded(base+4,1u)&&ok;
    if(!ok){uint ignored;InterlockedOr(sums[base+5],1u,ignored);InterlockedAdd(diagnostics[2],1u,ignored);}
}
