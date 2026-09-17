// Reprojects and accumulates one-bounce GI, AO, and shadow visibility. Motion vectors are in
// pixels from the previous location to the current location, so previousPixel = current - motion.
// Depth and normal rejection reset history at disocclusions.

cbuffer LightingTemporalConstants : register(b1) {
    uint gTemporalWidth;
    uint gTemporalHeight;
    uint gResetLightingHistory;
    uint gMaximumLightingHistory;
    float gLightingHistoryWeight;
    float gLightingDepthToleranceMeters;
    float gLightingNormalThreshold;
    float gTemporalPad0;
}

StructuredBuffer<float4> gCurrentIndirectDiffuse : register(t0);
StructuredBuffer<float> gCurrentAmbientOcclusion : register(t1);
StructuredBuffer<float> gCurrentShadowVisibility : register(t2);
StructuredBuffer<float4> gHistoryIndirectDiffuse : register(t3);
StructuredBuffer<float> gHistoryAmbientOcclusion : register(t4);
StructuredBuffer<float> gHistoryShadowVisibility : register(t5);
StructuredBuffer<float> gCurrentDepthMeters : register(t6);
StructuredBuffer<float> gHistoryDepthMeters : register(t7);
StructuredBuffer<float4> gCurrentNormals : register(t8);
StructuredBuffer<float4> gHistoryNormals : register(t9);
StructuredBuffer<float2> gMotionPixels : register(t10);
StructuredBuffer<uint> gLightingHistoryLength : register(t11);

RWStructuredBuffer<float4> gAccumulatedIndirectDiffuse : register(u0);
RWStructuredBuffer<float> gAccumulatedAmbientOcclusion : register(u1);
RWStructuredBuffer<float> gAccumulatedShadowVisibility : register(u2);
RWStructuredBuffer<uint> gUpdatedLightingHistoryLength : register(u3);

uint LightingPixelIndex(uint2 pixel) {
    return pixel.y * gTemporalWidth + pixel.x;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gTemporalWidth || pixel.y >= gTemporalHeight) return;
    uint index = LightingPixelIndex(pixel);

    float4 currentIndirect = gCurrentIndirectDiffuse[index];
    float currentAo = saturate(gCurrentAmbientOcclusion[index]);
    float currentShadow = saturate(gCurrentShadowVisibility[index]);
    bool acceptHistory = gResetLightingHistory == 0u;

    float2 previousPosition = float2(pixel) - gMotionPixels[index];
    int2 previousPixel = int2(round(previousPosition));
    acceptHistory = acceptHistory &&
        all(previousPixel >= int2(0, 0)) &&
        previousPixel.x < int(gTemporalWidth) && previousPixel.y < int(gTemporalHeight);

    uint previousIndex = index;
    if (acceptHistory) {
        previousIndex = LightingPixelIndex(uint2(previousPixel));
        float depthDelta = abs(gCurrentDepthMeters[index] - gHistoryDepthMeters[previousIndex]);
        float3 currentNormal = normalize(gCurrentNormals[index].xyz);
        float3 historyNormal = normalize(gHistoryNormals[previousIndex].xyz);
        acceptHistory = depthDelta <= max(0.0F, gLightingDepthToleranceMeters) &&
                        dot(currentNormal, historyNormal) >= saturate(gLightingNormalThreshold);
    }

    uint oldLength = acceptHistory ? gLightingHistoryLength[previousIndex] : 0u;
    uint maximumHistory = max(gMaximumLightingHistory, 1u);
    uint newLength = min(oldLength + 1u, maximumHistory);
    float historyWeight = acceptHistory
        ? saturate(gLightingHistoryWeight) *
          (float(oldLength) / float(max(oldLength + 1u, 1u)))
        : 0.0F;

    gAccumulatedIndirectDiffuse[index] = lerp(
        currentIndirect, gHistoryIndirectDiffuse[previousIndex], historyWeight);
    gAccumulatedAmbientOcclusion[index] = lerp(
        currentAo, saturate(gHistoryAmbientOcclusion[previousIndex]), historyWeight);
    gAccumulatedShadowVisibility[index] = lerp(
        currentShadow, saturate(gHistoryShadowVisibility[previousIndex]), historyWeight);
    gUpdatedLightingHistoryLength[index] = newLength;
}
