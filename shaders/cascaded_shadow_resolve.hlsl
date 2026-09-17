#include "common/cascaded_shadow_map.hlsli"

Texture2D<float> gCascadedShadowStaticDepth : register(t22);
Texture2D<float> gCascadedShadowDynamicDepth : register(t25);
SamplerComparisonState gCascadedShadowSampler : register(s2);
StructuredBuffer<float4> gCascadedShadowCoordinates : register(t23);
StructuredBuffer<float> gCascadedShadowViewDepth : register(t24);
RWStructuredBuffer<float> gCascadedShadowVisibility : register(u18);

cbuffer CascadedShadowConstants : register(b9) {
    float4 gCascadeSplitFarMeters;
    float4 gCascadeBlendStartMeters;
    float2 gCascadeTexelSize;
    uint gCascadeCount;
    uint gCascadePcfRadius;
    uint gCascadedShadowElementCount;
    uint gCascadedShadowPad0;
    uint gCascadedShadowPad1;
    uint gCascadedShadowPad2;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint i = dispatchThreadId.x;
    if (i >= gCascadedShadowElementCount) return;
    float depth = gCascadedShadowViewDepth[i];
    uint cascade = min(SelectShadowCascade(depth, gCascadeSplitFarMeters), max(gCascadeCount, 1u) - 1u);
    float3 coordinate = gCascadedShadowCoordinates[i * kMaximumShadowCascades + cascade].xyz;
    float staticVisibility = SampleCascadedShadowPcf(
        gCascadedShadowStaticDepth, gCascadedShadowSampler, coordinate, cascade,
        gCascadeCount, gCascadeTexelSize, gCascadePcfRadius);
    float dynamicVisibility = SampleCascadedShadowPcf(
        gCascadedShadowDynamicDepth, gCascadedShadowSampler, coordinate, cascade,
        gCascadeCount, gCascadeTexelSize, gCascadePcfRadius);
    float visibility = min(staticVisibility, dynamicVisibility);
    if (cascade + 1u < gCascadeCount) {
        float blendStart = gCascadeBlendStartMeters[cascade];
        float splitFar = gCascadeSplitFarMeters[cascade];
        float blend = CascadeBlendWeight(depth, blendStart, splitFar);
        if (blend > 0.0F) {
            float3 nextCoordinate = gCascadedShadowCoordinates[
                i * kMaximumShadowCascades + cascade + 1u].xyz;
            float nextStaticVisibility = SampleCascadedShadowPcf(
                gCascadedShadowStaticDepth, gCascadedShadowSampler, nextCoordinate, cascade + 1u,
                gCascadeCount, gCascadeTexelSize, gCascadePcfRadius);
            float nextDynamicVisibility = SampleCascadedShadowPcf(
                gCascadedShadowDynamicDepth, gCascadedShadowSampler, nextCoordinate, cascade + 1u,
                gCascadeCount, gCascadeTexelSize, gCascadePcfRadius);
            float nextVisibility = min(nextStaticVisibility, nextDynamicVisibility);
            visibility = lerp(visibility, nextVisibility, blend);
        }
    }
    gCascadedShadowVisibility[i] = visibility;
}
