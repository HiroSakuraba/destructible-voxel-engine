// Expand each visible-surface sample into four candidate world-space probe keys.
// Input/output records match split_rc_probe_assign_cs. The output of this pass
// can be passed directly to that shader; the caller still clears its hash table.
// This creates one nearest half-cell probe per surface per level. It is not yet
// the full interpolation-support / hierarchical allocation algorithm.
struct Sample { float4 positionSpacing; int4 levelSheet; };
[[vk::binding(0,0)]] StructuredBuffer<Sample> surfaces : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<Sample> candidates : register(u0);
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;surfaces.GetDimensions(count,stride);uint outCount;candidates.GetDimensions(outCount,stride);
    if(id.x>=count || id.x>=outCount/4u)return;
    const Sample original=surfaces[id.x];
    [unroll]for(uint level=0;level<4;++level){Sample s=original;s.levelSheet.x=(int)level;candidates[id.x*4u+level]=s;}
}
