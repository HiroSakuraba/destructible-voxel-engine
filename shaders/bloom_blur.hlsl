// Separable Gaussian blur: dispatched twice per widening step by the host (gBlurHorizontal=1 then
// gBlurHorizontal=0), rather than as a single-pass 2D convolution, because separable blur is
// O(2*radius) work per pixel instead of O(radius^2).
//
// Two corrections over the previous version:
//
// 1. Tap count now tracks sigma. It was min(8, ceil(radius)) against sigma = radius*0.5, so the
//    kernel was truncated at 2 sigma even in range (cutting ~4.6% of the energy at a visible
//    discontinuity), and above radius 8 the requested sigma kept growing while the tap count was
//    pinned. Measured effective sigma was 3.62 px at radius 8, 4.54 at radius 16 and 4.81 at
//    radius 32, with the outermost tap weight climbing to 0.88: the control was inert above 8 and
//    the Gaussian had degenerated into a hard edged box, which rings around highlights.
//
// 2. gBlurStride lets the host run this as a widening chain (stride 1, 2, 4, ...) to reach a wide
//    glow at bounded cost. A genuinely production grade bloom wants a downsampled mip pyramid;
//    this is the cheap approximation that fits the existing single buffer pair.

#include "common/render_environment.hlsli"

StructuredBuffer<float4> gBlurInput : register(t14);
RWStructuredBuffer<float4> gBlurOutput : register(u7);

cbuffer BlurConstants : register(b3) {
    uint gBlurHorizontal; // 1 = horizontal pass, 0 = vertical pass
    uint gBlurStride;     // tap spacing in pixels; 1 for a plain single step blur
    uint gBlurPad0;
    uint gBlurPad1;
}

static const uint kMaxBlurTaps = 24u; // each side; 49-tap kernel total at the maximum radius

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    uint pixelCount = gImageWidth * gImageHeight;
    if (pixelIndex >= pixelCount) return;

    uint px = pixelIndex % gImageWidth;
    uint py = pixelIndex / gImageWidth;

    uint stride = max(1u, gBlurStride);
    float sigma = max(gBloomRadius * 0.5F, 1.0e-3F);
    float twoSigmaSquared = 2.0F * sigma * sigma;
    // A Gaussian is truncated with no visible edge at three sigma. Taps are spaced `stride` pixels
    // apart, so the count needed is measured in strides.
    uint taps = clamp(uint(ceil(3.0F * sigma / float(stride))), 1u, kMaxBlurTaps);

    float3 accumulated = gBlurInput[pixelIndex].rgb; // centre tap, weight 1
    float totalWeight = 1.0F;

    [loop]
    for (uint i = 1u; i <= taps; ++i) {
        float distance = float(i * stride);
        float weight = exp(-(distance * distance) / twoSigmaSquared);

        int2 offset = gBlurHorizontal != 0u ? int2(int(i * stride), 0) : int2(0, int(i * stride));
        int2 forwardCoord = int2(px, py) + offset;
        int2 backwardCoord = int2(px, py) - offset;

        if (forwardCoord.x >= 0 && forwardCoord.x < int(gImageWidth) && forwardCoord.y >= 0 && forwardCoord.y < int(gImageHeight)) {
            accumulated += gBlurInput[uint(forwardCoord.y) * gImageWidth + uint(forwardCoord.x)].rgb * weight;
            totalWeight += weight;
        }
        if (backwardCoord.x >= 0 && backwardCoord.x < int(gImageWidth) && backwardCoord.y >= 0 && backwardCoord.y < int(gImageHeight)) {
            accumulated += gBlurInput[uint(backwardCoord.y) * gImageWidth + uint(backwardCoord.x)].rgb * weight;
            totalWeight += weight;
        }
    }

    gBlurOutput[pixelIndex] = float4(accumulated / max(totalWeight, 1.0e-5F), 1.0F);
}
