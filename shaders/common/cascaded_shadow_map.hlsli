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

#endif
