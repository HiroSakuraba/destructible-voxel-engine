// The step identified as missing when the shading pipeline was reviewed against what every
// major DCC/engine treats as non-negotiable: everything upstream of this file produces linear
// HDR radiance (correctly - that's what shade_primary.hlsl and composite_translucent.hlsl are
// supposed to output), and without this step, that radiance would go straight to a display
// that expects gamma-encoded [0,1] values. Bright/emissive/specular highlights would clip
// harshly to flat white and midtones would read as too dark - not a stylistic choice, a
// correctness bug.

#include "common/render_environment.hlsli"

StructuredBuffer<float4> gHdrColor : register(t13);
StructuredBuffer<float4> gBloomBuffer : register(t15); // already blurred (bloom_threshold.hlsl -> bloom_blur.hlsl x2)
RWStructuredBuffer<float4> gDisplayColor : register(u8);

float3 TonemapACES(float3 color) {
    // Narkowicz's fitted approximation of the ACES filmic curve - compact, no LUT needed, the
    // standard choice when a full ACES pipeline (RRT + ODT) isn't warranted.
    const float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 TonemapReinhard(float3 color) {
    return color / (1.0F.xxx + color);
}

float LinearToSrgbChannel(float value) {
    // The real sRGB OETF (a linear segment near black, then a power curve), not the common
    // pow(x, 1/2.2) shortcut: the two agree almost everywhere but visibly diverge near black,
    // exactly where banding/crushed-shadow artifacts would be most noticeable.
    if (value <= 0.0031308F) return value * 12.92F;
    return 1.055F * pow(max(value, 0.0F), 1.0F / 2.4F) - 0.055F;
}

float3 LinearToSrgb(float3 color) {
    return float3(LinearToSrgbChannel(color.x), LinearToSrgbChannel(color.y), LinearToSrgbChannel(color.z));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gImageWidth * gImageHeight) return;

    float4 hdrSample = gHdrColor[pixelIndex];
    float3 color = hdrSample.rgb * gGlobalTint * gExposure;
    // bloom_threshold.hlsl already applies gGlobalTint and gExposure, so the bloom buffer is
    // added in the same exposed linear space and must not be scaled by exposure again here.
    color += gBloomBuffer[pixelIndex].rgb * gBloomIntensity;

    float3 tonemapped;
    if (gTonemapOperator == kTonemapACES) tonemapped = TonemapACES(color);
    else if (gTonemapOperator == kTonemapReinhard) tonemapped = saturate(TonemapReinhard(color));
    else tonemapped = saturate(color); // kTonemapClamp

    gDisplayColor[pixelIndex] = float4(LinearToSrgb(tonemapped), saturate(hdrSample.a));
}
