// Reduces per-pixel shadow-ray results into one continuous directional-light visibility value.

#include "common/brick_trace_common.hlsli"
#include "common/render_environment.hlsli"

static const uint kMaximumShadowSamples = 16u;

StructuredBuffer<TraceResult> gShadowResults : register(t6);
RWStructuredBuffer<float> gShadowVisibility : register(u2);

cbuffer ShadowResolveConstants : register(b1) {
    uint gShadowPixelCount;
    uint gShadowResolvePad0;
    uint gShadowResolvePad1;
    uint gShadowResolvePad2;
}

float VisibilityFromOcclusion(float occlusion) {
    return saturate(1.0F - saturate(gShadowStrength) * saturate(occlusion));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gShadowPixelCount) return;
    uint baseIndex = pixelIndex * kMaximumShadowSamples;

    if (gShadowMode == kShadowOff) {
        gShadowVisibility[pixelIndex] = 1.0F;
        return;
    }
    if (gShadowMode == kShadowHard || gShadowMode == kShadowContact) {
        gShadowVisibility[pixelIndex] = VisibilityFromOcclusion(float(gShadowResults[baseIndex].hit));
        return;
    }

    uint softOffset = gShadowMode == kShadowHybrid ? 1u : 0u;
    uint softSamples = min(max(gShadowSamples, 1u), kMaximumShadowSamples - softOffset);
    uint occluded = 0u;
    [loop]
    for (uint sample = 0u; sample < softSamples; ++sample)
        occluded += gShadowResults[baseIndex + softOffset + sample].hit;
    float softOcclusion = float(occluded) / float(softSamples);
    if (gShadowMode == kShadowHybrid) {
        float contactOcclusion = float(gShadowResults[baseIndex].hit);
        softOcclusion = max(softOcclusion, contactOcclusion);
    }
    gShadowVisibility[pixelIndex] = VisibilityFromOcclusion(softOcclusion);
}
