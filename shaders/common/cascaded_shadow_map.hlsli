#ifndef DVE_CASCADED_SHADOW_MAP_HLSLI
#define DVE_CASCADED_SHADOW_MAP_HLSLI

static const uint kMaximumShadowCascades = 4u;

uint SelectShadowCascade(float viewDepth, float4 splitFarMeters) {
    if (viewDepth <= splitFarMeters.x) return 0u;
    if (viewDepth <= splitFarMeters.y) return 1u;
    if (viewDepth <= splitFarMeters.z) return 2u;
    return 3u;
}

float CascadeBlendWeight(float viewDepth, float blendStart, float splitFar) {
    return saturate((viewDepth - blendStart) / max(splitFar - blendStart, 1.0e-5F));
}

float SampleCascadedShadowPcf(
    Texture2D<float> shadowAtlas,
    SamplerComparisonState shadowSampler,
    float3 shadowUv,
    uint cascadeIndex,
    uint cascadeCount,
    float2 texelSize,
    uint radius) {
    uint columns = cascadeCount <= 1u ? 1u : 2u;
    uint rows = max(1u, (cascadeCount + columns - 1u) / columns);
    float2 tileSize = 1.0F.xx / float2(columns, rows);
    float2 tileOrigin = float2(cascadeIndex % columns, cascadeIndex / columns) * tileSize;
    float2 tileMaximum = tileOrigin + tileSize;
    if (any(shadowUv.xy < tileOrigin) || any(shadowUv.xy > tileMaximum) ||
        shadowUv.z < 0.0F || shadowUv.z > 1.0F) return 1.0F;
    // A gutterless atlas must clamp every comparison tap to the selected cascade's interior.
    float2 interiorMinimum = tileOrigin + 0.5F * texelSize;
    float2 interiorMaximum = tileMaximum - 0.5F * texelSize;
    float sum = 0.0F;
    uint count = 0u;
    int r = int(min(radius, 4u));
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            float2 tapUv = clamp(shadowUv.xy + float2(x, y) * texelSize,
                                 interiorMinimum, interiorMaximum);
            sum += shadowAtlas.SampleCmpLevelZero(shadowSampler, tapUv, shadowUv.z);
            ++count;
        }
    }
    return count > 0u ? sum / float(count) : 1.0F;
}

// ---- Bitmask soft shadows ------------------------------------------------------------------
// Schwarz & Stamminger, "Bitmask Soft Shadows" (2007), for a directional sun. Each texel in front
// of the receiver is projected back onto the sun disc as a small square; the 64-bit mask of disc
// samples it covers is OR-ed into one occlusion mask over every texel and both depth layers
// (static and dynamic). Visibility is the uncovered fraction of the disc. Because the masks are
// OR-ed, the light lost is the union of what each occluder hides, so two penumbrae that meet darken
// the gap between them more than either alone (the shadow blister). PCF cannot: it filters one
// depth comparison per texel and takes the darker of the two layers.
//
// The 64 samples form a rook pattern over [-1,1]^2: sample i has x slot i and y slot
// (19 i + 9) mod 64, slots centred at (slot + 0.5) / 32 - 1; the 52 inside the unit disc count.
// With bits in x-slot order, "x within [a, b]" is a run of bits and "y within [c, d]" is a
// difference of two prefix masks (kSunDiscYBelow), so one texel costs a few integer operations.
// Mirrored exactly by dve::render::bitmask_soft_shadow_visibility.
static const uint2 kSunDiscValid = uint2(0xFFFFDBB2u, 0x4DDBFFFFu);
// kSunDiscYBelow[k]: the samples whose y slot is below k (k = 0..64).
static const uint2 kSunDiscYBelow[65] = {
    uint2(0x00000000u, 0x00000000u), uint2(0x00002000u, 0x00000000u), uint2(0x00002000u, 0x00000100u), uint2(0x00002008u, 0x00000100u),
    uint2(0x40002008u, 0x00000100u), uint2(0x40002008u, 0x02000100u), uint2(0x40102008u, 0x02000100u), uint2(0x40102008u, 0x02008100u),
    uint2(0x40102408u, 0x02008100u), uint2(0x40102408u, 0x02008120u), uint2(0x40102409u, 0x02008120u), uint2(0x48102409u, 0x02008120u),
    uint2(0x48102409u, 0x02408120u), uint2(0x48122409u, 0x02408120u), uint2(0x48122409u, 0x02409120u), uint2(0x48122489u, 0x02409120u),
    uint2(0x48122489u, 0x02409124u), uint2(0x48122489u, 0x22409124u), uint2(0x49122489u, 0x22409124u), uint2(0x49122489u, 0x22489124u),
    uint2(0x49126489u, 0x22489124u), uint2(0x49126489u, 0x22489324u), uint2(0x49126499u, 0x22489324u), uint2(0xC9126499u, 0x22489324u),
    uint2(0xC9126499u, 0x26489324u), uint2(0xC9326499u, 0x26489324u), uint2(0xC9326499u, 0x26499324u), uint2(0xC9326C99u, 0x26499324u),
    uint2(0xC9326C99u, 0x26499364u), uint2(0xC9326C9Bu, 0x26499364u), uint2(0xD9326C9Bu, 0x26499364u), uint2(0xD9326C9Bu, 0x26C99364u),
    uint2(0xD9366C9Bu, 0x26C99364u), uint2(0xD9366C9Bu, 0x26C9B364u), uint2(0xD9366D9Bu, 0x26C9B364u), uint2(0xD9366D9Bu, 0x26C9B36Cu),
    uint2(0xD9366D9Bu, 0x66C9B36Cu), uint2(0xDB366D9Bu, 0x66C9B36Cu), uint2(0xDB366D9Bu, 0x66D9B36Cu), uint2(0xDB36ED9Bu, 0x66D9B36Cu),
    uint2(0xDB36ED9Bu, 0x66D9B76Cu), uint2(0xDB36EDBBu, 0x66D9B76Cu), uint2(0xDB36EDBBu, 0x66D9B76Du), uint2(0xDB36EDBBu, 0x6ED9B76Du),
    uint2(0xDB76EDBBu, 0x6ED9B76Du), uint2(0xDB76EDBBu, 0x6EDBB76Du), uint2(0xDB76FDBBu, 0x6EDBB76Du), uint2(0xDB76FDBBu, 0x6EDBB7EDu),
    uint2(0xDB76FDBFu, 0x6EDBB7EDu), uint2(0xFB76FDBFu, 0x6EDBB7EDu), uint2(0xFB76FDBFu, 0x6FDBB7EDu), uint2(0xFB7EFDBFu, 0x6FDBB7EDu),
    uint2(0xFB7EFDBFu, 0x6FDBF7EDu), uint2(0xFB7EFFBFu, 0x6FDBF7EDu), uint2(0xFB7EFFBFu, 0x6FDBF7FDu), uint2(0xFB7EFFBFu, 0xEFDBF7FDu),
    uint2(0xFF7EFFBFu, 0xEFDBF7FDu), uint2(0xFF7EFFBFu, 0xEFFBF7FDu), uint2(0xFF7FFFBFu, 0xEFFBF7FDu), uint2(0xFF7FFFBFu, 0xEFFBFFFDu),
    uint2(0xFF7FFFFFu, 0xEFFBFFFDu), uint2(0xFF7FFFFFu, 0xEFFBFFFFu), uint2(0xFF7FFFFFu, 0xFFFBFFFFu), uint2(0xFFFFFFFFu, 0xFFFBFFFFu),
    uint2(0xFFFFFFFFu, 0xFFFFFFFFu)
};

uint2 SunDiscBitsBelow(int n) {
    if (n <= 0) return uint2(0u, 0u);
    if (n < 32) return uint2((1u << uint(n)) - 1u, 0u);
    if (n < 64) return uint2(0xFFFFFFFFu, (1u << uint(n - 32)) - 1u);
    return uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
}

// The disc samples inside the box [lo, hi] (disc units: the sun disc is the unit disc).
uint2 SunDiscBox(float2 lo, float2 hi) {
    int x0 = max(int(ceil(32.0F * (lo.x + 1.0F) - 0.5F)), 0);
    int x1 = min(int(floor(32.0F * (hi.x + 1.0F) - 0.5F)), 63);
    int y0 = max(int(ceil(32.0F * (lo.y + 1.0F) - 0.5F)), 0);
    int y1 = min(int(floor(32.0F * (hi.y + 1.0F) - 0.5F)), 63);
    if (x0 > x1 || y0 > y1) return uint2(0u, 0u);
    uint2 xs = SunDiscBitsBelow(x1 + 1) & ~SunDiscBitsBelow(x0);
    uint2 ys = kSunDiscYBelow[y1 + 1] & ~kSunDiscYBelow[y0];
    return xs & ys;
}

uint SunDiscCount(uint2 mask) {
    return countbits(mask.x) + countbits(mask.y);
}

// Visibility of the sun disc (radius = atan(tanSunRadius)) at a receiver, from the static and
// dynamic depth atlases. texelMeters and depthRangeMeters are the cascade's texel size and depth
// range; searchTexels caps how far (in texels) blockers are looked for, which caps the penumbra
// width at about 2 x searchTexels texels.
float SampleCascadedShadowBitmask(
    Texture2D<float> staticAtlas,
    Texture2D<float> dynamicAtlas,
    float3 shadowUv,
    uint cascadeIndex,
    uint cascadeCount,
    float2 texelSize,
    float texelMeters,
    float depthRangeMeters,
    float tanSunRadius,
    uint searchTexels) {
    uint columns = cascadeCount <= 1u ? 1u : 2u;
    uint rows = max(1u, (cascadeCount + columns - 1u) / columns);
    float2 tileSize = 1.0F.xx / float2(columns, rows);
    float2 tileOrigin = float2(cascadeIndex % columns, cascadeIndex / columns) * tileSize;
    float2 tileMaximum = tileOrigin + tileSize;
    if (any(shadowUv.xy < tileOrigin) || any(shadowUv.xy > tileMaximum) ||
        shadowUv.z < 0.0F || shadowUv.z > 1.0F) return 1.0F;
    if (!(tanSunRadius > 0.0F) || !(texelMeters > 0.0F) || !(depthRangeMeters > 0.0F)) return 1.0F;
    // Continuous texel coordinates (texel centres at i + 0.5) and the cascade's texel rectangle.
    float2 texel = shadowUv.xy / texelSize;
    int2 tileFirst = int2(round(tileOrigin / texelSize));
    int2 tileLast = int2(round(tileMaximum / texelSize)) - 1;
    int2 centre = int2(floor(texel));
    int cap = int(min(searchTexels, 16u));

    // 1. Nearest blocker over the search square (every other texel): sizes the real search.
    float nearest = shadowUv.z;
    [loop]
    for (int sy = -cap; sy <= cap; sy += 2) {
        [loop]
        for (int sx = -cap; sx <= cap; sx += 2) {
            int2 tap = centre + int2(sx, sy);
            if (any(tap < tileFirst) || any(tap > tileLast)) continue;
            nearest = min(nearest, min(staticAtlas.Load(int3(tap, 0)), dynamicAtlas.Load(int3(tap, 0))));
        }
    }
    if (!(nearest < shadowUv.z)) return 1.0F;
    float reach = (shadowUv.z - nearest) * depthRangeMeters * tanSunRadius;
    int radius = min(cap, int(ceil(reach / texelMeters + 0.5F)));

    // 2. Back-project every texel in front of the receiver onto the sun disc.
    uint2 occluded = uint2(0u, 0u);
    float halfTexel = 0.5F * texelMeters;
    [loop]
    for (int y = -radius; y <= radius; ++y) {
        [loop]
        for (int x = -radius; x <= radius; ++x) {
            int2 tap = centre + int2(x, y);
            if (any(tap < tileFirst) || any(tap > tileLast)) continue;
            float2 offset = (float2(tap) + 0.5F - texel) * texelMeters;
            float depths[2] = { staticAtlas.Load(int3(tap, 0)), dynamicAtlas.Load(int3(tap, 0)) };
            [unroll]
            for (int layer = 0; layer < 2; ++layer) {
                float depth = depths[layer];
                if (!(depth < shadowUv.z)) continue;
                // The sun disc seen from the receiver, on this texel's depth plane, has radius
                // footprint metres; the texel covers (offset +- halfTexel) / footprint of it.
                float footprint = max((shadowUv.z - depth) * depthRangeMeters * tanSunRadius, 1.0e-9F);
                occluded |= SunDiscBox((offset - halfTexel) / footprint, (offset + halfTexel) / footprint);
            }
        }
    }
    return float(SunDiscCount(kSunDiscValid & ~occluded)) / float(SunDiscCount(kSunDiscValid));
}

#endif
