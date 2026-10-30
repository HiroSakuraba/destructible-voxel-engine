// First Split Radiance Cascades building block: split a traced surface ray into
// four interval samples. This pass is not a complete GI renderer: it consumes
// already-traced rays and intentionally performs no probe allocation or merging.
// A negative distance is a miss; hits at or beyond the last cutoff use the sky.
struct SplitRay { float4 hitRadianceDistance; float4 skyRadianceBaseCutoff; };
[[vk::binding(0,0)]] StructuredBuffer<SplitRay> rays : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<float4> intervals : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count, stride; rays.GetDimensions(count, stride);
    if (id.x >= count) return;
    const SplitRay ray = rays[id.x];
    const float distance = ray.hitRadianceDistance.w;
    float cutoff = ray.skyRadianceBaseCutoff.w;
    uint terminal = 3;
    bool hit = distance >= 0.0F;
    [unroll] for (uint level=0; level<4; ++level) {
        if (hit && distance < cutoff) { terminal = level; break; }
        cutoff *= 4.0F;
    }
    // A hit outside the represented range is a miss for this bounded ray.
    if (hit && distance >= ray.skyRadianceBaseCutoff.w * 64.0F) hit = false;
    [unroll] for (uint level=0; level<4; ++level) {
        float4 value = float4(0,0,0,-1); // no sample beyond termination
        if (level < terminal) value = float4(0,0,0,1);
        if (level == terminal) value = float4(hit ? ray.hitRadianceDistance.xyz : ray.skyRadianceBaseCutoff.xyz,0);
        intervals[id.x*4 + level] = value;
    }
}
