// Final gather of the radiance cascades: each pixel blends the four nearest level-0 probes
// (bilateral weights) and integrates each probe's direction map with the cosine around the
// pixel's own normal. The result is what a white diffuse surface would draw, like
// player_traced_gi_ps.
#include "common/player_rc.hlsli"

float3 GatherProbe(int2 probe, float3 normal, RcProbe info) {
    const int dirRes = (int)gRcLevel.y;
    float3 sum = 0.0F.xxx;
    float total = 0.0F;
    for (int y = 0; y < dirRes; ++y) {
        for (int x = 0; x < dirRes; ++x) {
            const float2 dirUv = (float2(int2(x, y)) + 0.5F) / float(dirRes);
            const float3 d = OctahedralDirection(dirUv);
            if (dot(d, info.normal) <= 0.0F) continue;   // not traced from this probe
            const float weight = OctahedralTexelWeight(dirUv, float(dirRes));
            const float4 radiance = gRcUpper.Load(int3(probe * dirRes + int2(x, y), 0));
            const float cosine = max(dot(d, normal), 0.0F);
            sum += radiance.rgb * (cosine * weight);
            total += cosine * weight;
        }
    }
    return total > 1.0e-5F ? sum / total : 0.0F.xxx;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    float3 centre, normal;
    if (gGridOrigin.w <= 0.0F || !SurfaceAtPixel(pixel, centre, normal)) return float4(0.0F, 0.0F, 0.0F, 0.0F);
    const float spacing = gRcLevel.z;
    const float2 probeUv = (float2(pixel) + 0.5F) / spacing - 0.5F;
    const int2 base = int2(floor(probeUv));
    const float2 frac = probeUv - float2(base);
    const float sigma = 2.0F * spacing * PixelFootprint(centre);
    float3 sum = 0.0F.xxx;
    float totalWeight = 0.0F;
    float3 plainSum = 0.0F.xxx;
    float plainWeight = 0.0F;
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 offset = int2(k & 1, k >> 1);
        const int2 probe = base + offset;
        if (any(probe < 0) || any(probe >= int2(gRcProbes.xy))) continue;
        const float bilinear = (offset.x != 0 ? frac.x : 1.0F - frac.x) * (offset.y != 0 ? frac.y : 1.0F - frac.y);
        const RcProbe info = LoadProbe(probe, spacing);
        if (!info.valid) continue;
        const float3 radiance = GatherProbe(probe, normal, info);
        const float w = bilinear * ProbeCompatibility(centre, normal, info, sigma);
        sum += radiance * w;
        totalWeight += w;
        plainSum += radiance * bilinear;
        plainWeight += bilinear;
    }
    if (totalWeight > 1.0e-4F) return float4(sum / totalWeight, 1.0F);
    if (plainWeight > 1.0e-4F) return float4(plainSum / plainWeight, 1.0F);
    return float4(SkyBlend(normal), 1.0F);
}
