// One radiance-cascade level (see common/player_rc.hlsli). Each texel is one probe direction:
// trace the level's ray interval through the scene voxel grid; on a hit use the one-bounce
// radiance of the voxel, on a miss continue with the level above (or the sky on the top level).
#include "common/player_rc.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 texel = int2(position.xy);
    const int dirRes = (int)gRcLevel.y;
    const int2 probeIndex = texel / dirRes;
    const int2 dirIndex = texel - probeIndex * dirRes;
    const float spacing = gRcLevel.z;
    if (gGridOrigin.w <= 0.0F || any(probeIndex >= int2(gRcProbes.xy))) return float4(0.0F, 0.0F, 0.0F, 0.0F);
    const RcProbe probe = LoadProbe(probeIndex, spacing);
    if (!probe.valid) return float4(0.0F, 0.0F, 0.0F, 0.0F);

    const float2 dirUv = (float2(dirIndex) + 0.5F) / float(dirRes);
    const float3 direction = OctahedralDirection(dirUv);
    if (dot(direction, probe.normal) <= 0.0F) return float4(0.0F, 0.0F, 0.0F, 1.0F);

    const float lift = max(max(1.0e-3F, gParams.z), 1.1F * gGridOrigin.w);
    const float3 origin = probe.position + probe.normal * lift;
    const float start = gRcInterval.x;
    const float span = max(gRcInterval.y - start, 1.0e-3F);
    GridHit hit;
    if (TraceSceneGrid(origin + direction * start, direction, span, hit)) {
        const float3 hitPoint = origin + direction * (start + hit.t);
        return float4(BounceRadiance(hitPoint, hit, lift), 1.0F);
    }
    if (gRcInterval.w > 0.5F) return float4(SkyBlend(direction), 1.0F);

    // Continue with the level above: the four probes around this one, bilateral weights, and
    // for each the 2x2 texels of its finer direction map that cover this texel.
    const float upperSpacing = spacing * 2.0F;
    const float upperDirRes = gRcInterval.z;
    const float2 probeUv = (float2(probeIndex) + 0.5F) * 0.5F - 0.5F;   // in upper probe units
    const int2 base = int2(floor(probeUv));
    const float2 frac = probeUv - float2(base);
    const float sigma = 2.0F * upperSpacing * PixelFootprint(probe.position);
    float3 sum = 0.0F.xxx;
    float totalWeight = 0.0F;
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 offset = int2(k & 1, k >> 1);
        const int2 upperProbe = base + offset;
        if (any(upperProbe < 0) || any(upperProbe >= int2(gRcProbes.zw))) continue;
        const float bilinear = (offset.x != 0 ? frac.x : 1.0F - frac.x) * (offset.y != 0 ? frac.y : 1.0F - frac.y);
        const RcProbe other = LoadProbe(upperProbe, upperSpacing);
        if (!other.valid) continue;
        float3 radiance = 0.0F.xxx;
        [unroll] for (int j = 0; j < 4; ++j) {
            const int2 sub = int2(j & 1, j >> 1);
            const int2 coord = upperProbe * (int)upperDirRes + dirIndex * 2 + sub;
            radiance += gRcUpper.Load(int3(coord, 0)).rgb * 0.25F;
        }
        const float w = bilinear * ProbeCompatibility(probe.position, probe.normal, other, sigma);
        sum += radiance * w;
        totalWeight += w;
    }
    if (totalWeight > 1.0e-3F) return float4(sum / totalWeight, 1.0F);
    // No compatible probe above (a silhouette, an isolated surface, the screen edge): carry this
    // ray on to the GI distance instead.
    const float rest = max(gSkyColor.w - gRcInterval.y, 0.0F);
    if (rest > 1.0e-3F && TraceSceneGrid(origin + direction * gRcInterval.y, direction, rest, hit))
        return float4(BounceRadiance(origin + direction * (gRcInterval.y + hit.t), hit, lift), 1.0F);
    return float4(SkyBlend(direction), 1.0F);
}
