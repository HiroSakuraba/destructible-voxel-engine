// Player split-RC merge with per-probe visible-sample weighting.
// `keys` is the current populated probe hash table. `offsets` provides the
// compact per-level directional slice for each slot. Each parent level must be
// completed before running its children. Alpha in intervals is beta (-1 means missing), while
// alpha in parents/outputs is validity (0 or 1). Query = (slot,direction).
// A missing slice or parent remains unavailable.
[[vk::binding(0,0)]] StructuredBuffer<uint> keys : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<float4> intervals : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<float4> parents : register(t2);
[[vk::binding(3,0)]] StructuredBuffer<uint2> queries : register(t3);
[[vk::binding(4,0)]] RWStructuredBuffer<float4> output : register(u0);
[[vk::binding(5,0)]] StructuredBuffer<uint> offsets : register(t4);
[[vk::binding(6,0)]] StructuredBuffer<uint> probeCounts : register(t5);
uint HashKey(uint k){k^=k>>16;k*=0x7feb352d;k^=k>>15;k*=0x846ca68b;return k^(k>>16);}
uint PackKey(int3 cell,uint level,uint sheet){
    const uint3 c=(uint3)(cell+128);
    return (c.x|(c.y<<8)|(c.z<<16)|(level<<24)|(sheet<<26))+1u;
}
uint Lookup(uint key,uint capacity){
    const uint start=HashKey(key)%capacity;
    [loop]for(uint i=0;i<capacity;++i){const uint slot=(start+i)%capacity;const uint value=keys[slot];
        if(value==key)return slot;if(value==0u)return 0xffffffffu;}
    return 0xffffffffu;
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint count,stride;queries.GetDimensions(count,stride);uint outputCount;output.GetDimensions(outputCount,stride);
    if(id.x>=count || id.x>=outputCount)return;
    output[id.x]=float4(0,0,0,0);
    uint capacity;keys.GetDimensions(capacity,stride);uint intervalCount;intervals.GetDimensions(intervalCount,stride);
    uint parentCount;parents.GetDimensions(parentCount,stride);
    uint offsetCount;offsets.GetDimensions(offsetCount,stride);
    const uint2 q=queries[id.x];
    if(q.x>=capacity || q.x>=offsetCount)return;
    const uint stored=keys[q.x];if(stored==0u)return;
    const uint packed=stored-1u,level=(packed>>24)&3u,sheet=(packed>>26)&7u;
    if(sheet>=6u || q.y >= (32u << (2u*level)))return;
    const uint childOffset=offsets[q.x];
    if(childOffset==0xffffffffu || childOffset>=intervalCount || q.y>=intervalCount-childOffset)return;
    const float4 local=intervals[childOffset+q.y];
    if(!all(isfinite(local)) || local.w<0 || local.w>1 || any(local.xyz<0))return;
    if(local.w==0){output[id.x]=float4(local.xyz,1);return;}
    if(level>=3u || capacity==0u)return;
    const int3 cell=int3(int(packed&255u),int((packed>>8)&255u),int((packed>>16)&255u))-128;
    // Coordinates in the parent half-cell-centred lattice, including negatives.
    const float3 p=(float3(cell)+0.5F)*0.5F-0.5F;
    const int3 base=(int3)floor(p);const float3 f=p-float3(base);
    float3 sum=0;float total=0;
    [unroll]for(uint i=0;i<8;++i){
        const int3 o=int3(int(i&1u),int((i>>1)&1u),int((i>>2)&1u));const int3 c=base+o;
        if(any(c< -128)||any(c>127))continue;
        const float3 w=lerp(1.0F-f,f,float3(o));const float spatialWeight=w.x*w.y*w.z;
        const uint slot=Lookup(PackKey(c,level+1u,sheet),capacity);
        if(slot==0xffffffffu||slot>=offsetCount)continue;
        const uint parentOffset=offsets[slot],firstDir=q.y*4u;
        if(parentOffset==0xffffffffu || parentOffset>=parentCount || firstDir>=parentCount-parentOffset ||
           parentCount-parentOffset-firstDir<4u)continue;
        uint probeCount,stride;probeCounts.GetDimensions(probeCount,stride);
        if(slot>=probeCount||probeCounts[slot]==0u)continue;
        float3 angularSum=0;float angularCount=0;
        [unroll]for(uint j=0;j<4;++j){const float4 v=parents[parentOffset+firstDir+j];
            if(v.w>0.5F&&all(isfinite(v))&&all(v.xyz>=0)){angularSum+=v.xyz;angularCount+=1.0F;}}
        if(angularCount>0){const float weight=spatialWeight*(float)probeCounts[slot];sum+=(angularSum/angularCount)*weight;total+=weight;}
    }
    if(total>1e-7F)output[id.x]=float4(local.xyz+local.w*sum/total,1);
}
