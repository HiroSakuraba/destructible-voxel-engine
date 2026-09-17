// The other half of real alpha transparency (see trace_translucent.hlsl for the tracing side):
// standard front-to-back "over" compositing across up to kMaxTranslucentLayers per pixel, each
// layer already independently shaded by shade_primary.hlsl (once per layer, same shader, no
// separate translucency-specific lighting code). This is the same compositing math used by
// essentially every layered-transparency technique (order-independent or not); the only
// engine-specific part of this whole translucency feature is trace_translucent.hlsl's use of
// ignoreMaterial to find each successive layer.

static const uint kMaxTranslucentLayers = 4u; // must match trace_translucent.hlsl

StructuredBuffer<float4> gLayerRadiance : register(t11); // pixelIndex * kMaxTranslucentLayers + layerIndex
StructuredBuffer<uint> gLayerCount : register(t12);      // valid layer count per pixel, <= kMaxTranslucentLayers
RWStructuredBuffer<float4> gFinalColor : register(u5);

cbuffer CompositeConstants : register(b1) {
    uint gPixelCount;
    uint gCompositePad0;
    uint gCompositePad1;
    uint gCompositePad2;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint pixelIndex = dispatchThreadId.x;
    if (pixelIndex >= gPixelCount) return;

    uint layerCount = min(gLayerCount[pixelIndex], kMaxTranslucentLayers);
    float3 accumulatedColor = float3(0.0F, 0.0F, 0.0F);
    float accumulatedAlpha = 0.0F;

    for (uint layer = 0u; layer < layerCount; ++layer) {
        float4 sample = gLayerRadiance[pixelIndex * kMaxTranslucentLayers + layer];
        float remainingTransmittance = 1.0F - accumulatedAlpha;
        if (remainingTransmittance <= 0.0F) break; // fully opaque already: further (occluded) layers cannot contribute
        accumulatedColor += sample.rgb * sample.a * remainingTransmittance;
        accumulatedAlpha += sample.a * remainingTransmittance;
    }

    gFinalColor[pixelIndex] = float4(accumulatedColor, saturate(accumulatedAlpha));
}
