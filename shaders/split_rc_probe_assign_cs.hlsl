// Experimental Split RC sparse probe assignment. Each valid sample inserts a
// world-cell/level/surface-sheet key into a zero-cleared uint hash table.
// slots and counters must be cleared before dispatch; assignment indices remain
// valid only for this table's lifetime, while the key is stable across rebuilds.
// Each sample supplies a positive finite base spacing shared by the scene.
struct Sample { float4 positionSpacing; int4 levelSheet; };
[[vk::binding(0,0)]] StructuredBuffer<Sample> samples : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<uint> slots : register(u0);
[[vk::binding(2,0)]] RWStructuredBuffer<uint> result : register(u1);
// counters[0] = out-of-range or invalid samples, counters[1] = full table
[[vk::binding(3,0)]] RWStructuredBuffer<uint> counters : register(u2);
uint hashKey(uint k) { k ^= k >> 16; k *= 0x7feb352d; k ^= k >> 15; k *= 0x846ca68b; return k ^ (k >> 16); }
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count, stride; samples.GetDimensions(count,stride);
    if (id.x >= count) return;
    result[id.x] = 0xffffffffu;
    const Sample s = samples[id.x];
    const float base = s.positionSpacing.w;
    const int level = s.levelSheet.x, sheet = s.levelSheet.y;
    if (!all(isfinite(s.positionSpacing)) || base <= 0 || level < 0 || level > 3 || sheet < 0 || sheet > 5) {
        uint ignored; InterlockedAdd(counters[0],1,ignored); return;
    }
    const float spacing = base * (float)(1u << level);
    if (!isfinite(spacing) || spacing <= 0) { uint ignored; InterlockedAdd(counters[0],1,ignored); return; }
    const float3 cellFloat = floor(s.positionSpacing.xyz / spacing);
    if (!all(isfinite(cellFloat)) || any(cellFloat < -128.0F) || any(cellFloat >= 128.0F)) {
        uint ignored; InterlockedAdd(counters[0],1,ignored); return;
    }
    const uint3 cell = (uint3)((int3)cellFloat + 128);
    // 24 cell bits, 2 level bits, 3 sheet bits. +1 reserves zero as empty.
    const uint key = (cell.x | (cell.y << 8) | (cell.z << 16) | ((uint)level << 24) | ((uint)sheet << 26)) + 1u;
    uint capacity; slots.GetDimensions(capacity,stride);
    if (capacity == 0) { uint ignored; InterlockedAdd(counters[1],1,ignored); return; }
    const uint start = hashKey(key) % capacity;
    [loop] for (uint probe=0; probe<capacity; ++probe) {
        const uint index = (start+probe) % capacity;
        uint previous; InterlockedCompareExchange(slots[index],0u,key,previous);
        if (previous == 0u || previous == key) { result[id.x]=index; return; }
    }
    uint ignored; InterlockedAdd(counters[1],1,ignored);
}
