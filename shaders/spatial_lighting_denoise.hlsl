// A compact 5x5 cross-bilateral filter for temporally accumulated GI, AO, and shadows.
// Depth and normal weights preserve geometric edges while the spatial Gaussian removes residual
// low-sample noise. Shadows blur no wider than a quarter of the penumbra under the filter, so
// narrow penumbrae (contact shadows, the blister where two penumbrae meet) keep their shape while
// wide ones are still smoothed. Mirrored by dve::render::denoise_shadow_visibility.

cbuffer LightingDenoiseConstants : register(b1) {
    uint gDenoiseWidth;
    uint gDenoiseHeight;
    uint gDenoiseRadius;
    uint gDenoisePad0;
    float gDenoiseSpatialSigma;
    float gDenoiseDepthSigmaMeters;
    float gDenoiseNormalPower;
    // World angle covered by one pixel at the centre of the view (radians). 0 keeps the old
    // fixed shadow blur (gDenoiseSpatialSigma), for hosts that do not set it.
    float gDenoiseRadiansPerPixel;
}

StructuredBuffer<float4> gTemporalIndirectDiffuse : register(t0);
StructuredBuffer<float> gTemporalAmbientOcclusion : register(t1);
StructuredBuffer<float> gTemporalShadowVisibility : register(t2);
StructuredBuffer<float> gDenoiseDepthMeters : register(t3);
StructuredBuffer<float4> gDenoiseNormals : register(t4);
StructuredBuffer<float> gDenoiseShadowPenumbraMeters : register(t5);

RWStructuredBuffer<float4> gFilteredIndirectDiffuse : register(u0);
RWStructuredBuffer<float> gFilteredAmbientOcclusion : register(u1);
RWStructuredBuffer<float> gFilteredShadowVisibility : register(u2);

uint DenoisePixelIndex(uint2 pixel) {
    return pixel.y * gDenoiseWidth + pixel.x;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gDenoiseWidth || pixel.y >= gDenoiseHeight) return;
    uint index = DenoisePixelIndex(pixel);

    float centerDepth = gDenoiseDepthMeters[index];
    float3 centerNormal = normalize(gDenoiseNormals[index].xyz);
    float spatialSigma = max(gDenoiseSpatialSigma, 0.25F);
    float depthSigma = max(gDenoiseDepthSigmaMeters, 1.0e-4F);
    int radius = int(min(gDenoiseRadius, 2u));

    // The widest penumbra under the filter, in pixels at this depth.
    float shadowSigma = spatialSigma;
    if (gDenoiseRadiansPerPixel > 0.0F) {
        float penumbraMeters = 0.0F;
        [loop]
        for (int py = -radius; py <= radius; ++py) {
            [loop]
            for (int px = -radius; px <= radius; ++px) {
                int2 tap = clamp(int2(pixel) + int2(px, py), int2(0, 0),
                                 int2(int(gDenoiseWidth) - 1, int(gDenoiseHeight) - 1));
                penumbraMeters = max(penumbraMeters, gDenoiseShadowPenumbraMeters[DenoisePixelIndex(uint2(tap))]);
            }
        }
        float pixelMeters = max(centerDepth, 1.0e-3F) * gDenoiseRadiansPerPixel;
        shadowSigma = clamp(0.25F * penumbraMeters / pixelMeters, 0.25F, spatialSigma);
    }

    float4 indirectSum = 0.0F.xxxx;
    float aoSum = 0.0F;
    float shadowSum = 0.0F;
    float shadowWeightSum = 0.0F;
    float weightSum = 0.0F;
    [loop]
    for (int y = -radius; y <= radius; ++y) {
        [loop]
        for (int x = -radius; x <= radius; ++x) {
            int2 tapPixel = clamp(int2(pixel) + int2(x, y), int2(0, 0),
                                  int2(int(gDenoiseWidth) - 1, int(gDenoiseHeight) - 1));
            uint tapIndex = DenoisePixelIndex(uint2(tapPixel));
            float tapDepth = gDenoiseDepthMeters[tapIndex];
            float3 tapNormal = normalize(gDenoiseNormals[tapIndex].xyz);
            float spatialWeight = exp(-float(x * x + y * y) /
                                      (2.0F * spatialSigma * spatialSigma));
            float depthWeight = exp(-abs(tapDepth - centerDepth) / depthSigma);
            float normalWeight = pow(saturate(dot(centerNormal, tapNormal)),
                                     max(gDenoiseNormalPower, 1.0F));
            float weight = spatialWeight * depthWeight * normalWeight;
            float shadowWeight = exp(-float(x * x + y * y) / (2.0F * shadowSigma * shadowSigma)) *
                                 depthWeight * normalWeight;
            indirectSum += gTemporalIndirectDiffuse[tapIndex] * weight;
            aoSum += saturate(gTemporalAmbientOcclusion[tapIndex]) * weight;
            shadowSum += saturate(gTemporalShadowVisibility[tapIndex]) * shadowWeight;
            shadowWeightSum += shadowWeight;
            weightSum += weight;
        }
    }

    float inverseWeight = 1.0F / max(weightSum, 1.0e-6F);
    gFilteredIndirectDiffuse[index] = indirectSum * inverseWeight;
    gFilteredAmbientOcclusion[index] = aoSum * inverseWeight;
    gFilteredShadowVisibility[index] = shadowSum / max(shadowWeightSum, 1.0e-6F);
}
