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

#include "common/tonemap_curves.hlsli"

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
    else if (gTonemapOperator == kTonemapAgX) tonemapped = saturate(TonemapAgX(color));
    else if (gTonemapOperator == kTonemapPBRNeutral) tonemapped = saturate(TonemapPBRNeutral(color));
    else tonemapped = saturate(color); // kTonemapClamp

    gDisplayColor[pixelIndex] = float4(LinearToSrgb(tonemapped), saturate(hdrSample.a));
}
