// First stage of bloom: a soft threshold (Unreal's own technique - a smooth knee rather than a
// hard cutoff, which avoids a visible edge where pixels cross the threshold) applied to the
// linear HDR radiance. Output feeds bloom_blur.hlsl (dispatched as a widening chain) and is added
// back into the final image by tonemap.hlsl.
//
// Exposure is applied here, before the threshold. Thresholding unexposed radiance and adding the
// result after exposure meant that (a) which highlights bloomed did not change when the camera was
// stopped down, and (b) bloom strength scaled as 1/exposure. tonemap.hlsl therefore must not apply
// gExposure to the bloom buffer a second time.

#include "common/render_environment.hlsli"

StructuredBuffer<float4> gHdrColor : register(t13);
RWStructuredBuffer<float4> gBloomBuffer : register(u6);

float3 SoftThreshold(float3 color, float threshold) {
    float brightness = max(color.r, max(color.g, color.b));
    float knee = threshold * 0.5F;
    float soft = brightness - threshold + knee;
    soft = clamp(soft, 0.0F, 2.0F * knee);
    soft = soft * soft / max(4.0F * knee, 1.0e-5F);
    float contribution = max(soft, brightness - threshold);
    return color * (contribution / max(brightness, 1.0e-5F));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gImageWidth * gImageHeight) return;
    float4 hdr = gHdrColor[pixelIndex];
    float3 exposed = hdr.rgb * gGlobalTint * gExposure;
    gBloomBuffer[pixelIndex] = float4(SoftThreshold(exposed, gBloomThreshold), 1.0F);
}
