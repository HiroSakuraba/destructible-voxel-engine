#include "common/cascaded_shadow_map.hlsli"

Texture2D<float> gCascadedShadowStaticDepth : register(t22);
Texture2D<float> gCascadedShadowDynamicDepth : register(t25);
SamplerComparisonState gCascadedShadowSampler : register(s2);
StructuredBuffer<float4> gCascadedShadowCoordinates : register(t23);
StructuredBuffer<float> gCascadedShadowViewDepth : register(t24);
RWStructuredBuffer<float> gCascadedShadowVisibility : register(u18);

// Mirrored by dve::render::GpuCascadedShadowConstants (cascaded_shadow_map.hpp).
cbuffer CascadedShadowConstants : register(b9) {
    float4 gCascadeSplitFarMeters;
    float4 gCascadeBlendStartMeters;
    float2 gCascadeTexelSize;
    uint gCascadeCount;
    uint gCascadePcfRadius;
    uint gCascadedShadowElementCount;
    // 0: PCF (fixed kernel, darker of the two layers). 1: bitmask soft shadows (sun-disc union).
    uint gCascadeSoftShadowFilter;
    uint gCascadeSoftShadowSearchTexels;
    float gCascadeTanSunAngularRadius;
    float4 gCascadeTexelMeters;        // per cascade
    float4 gCascadeDepthRangeMeters;   // per cascade: maximum - minimum light depth
}

float CascadeVisibility(float3 coordinate, uint cascade) {
    if (gCascadeSoftShadowFilter == 1u)
        return SampleCascadedShadowBitmask(
            gCascadedShadowStaticDepth, gCascadedShadowDynamicDepth, coordinate, cascade, gCascadeCount,
            gCascadeTexelSize, gCascadeTexelMeters[cascade], gCascadeDepthRangeMeters[cascade],
            gCascadeTanSunAngularRadius, gCascadeSoftShadowSearchTexels);
    float staticVisibility = SampleCascadedShadowPcf(
        gCascadedShadowStaticDepth, gCascadedShadowSampler, coordinate, cascade,
        gCascadeCount, gCascadeTexelSize, gCascadePcfRadius);
    float dynamicVisibility = SampleCascadedShadowPcf(
        gCascadedShadowDynamicDepth, gCascadedShadowSampler, coordinate, cascade,
        gCascadeCount, gCascadeTexelSize, gCascadePcfRadius);
    return min(staticVisibility, dynamicVisibility);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint i = dispatchThreadId.x;
    if (i >= gCascadedShadowElementCount) return;
    float depth = gCascadedShadowViewDepth[i];
    uint cascade = min(SelectShadowCascade(depth, gCascadeSplitFarMeters), max(gCascadeCount, 1u) - 1u);
    float3 coordinate = gCascadedShadowCoordinates[i * kMaximumShadowCascades + cascade].xyz;
    float visibility = CascadeVisibility(coordinate, cascade);
    if (cascade + 1u < gCascadeCount) {
        float blendStart = gCascadeBlendStartMeters[cascade];
        float splitFar = gCascadeSplitFarMeters[cascade];
        float blend = CascadeBlendWeight(depth, blendStart, splitFar);
        if (blend > 0.0F) {
            float3 nextCoordinate = gCascadedShadowCoordinates[
                i * kMaximumShadowCascades + cascade + 1u].xyz;
            visibility = lerp(visibility, CascadeVisibility(nextCoordinate, cascade + 1u), blend);
        }
    }
    gCascadedShadowVisibility[i] = visibility;
}
