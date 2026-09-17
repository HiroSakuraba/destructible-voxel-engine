// A compact 5x5 cross-bilateral filter for temporally accumulated GI, AO, and shadows.
// Depth and normal weights preserve geometric edges while the spatial Gaussian removes residual
// low-sample noise.

cbuffer LightingDenoiseConstants : register(b1) {
    uint gDenoiseWidth;
    uint gDenoiseHeight;
    uint gDenoiseRadius;
    uint gDenoisePad0;
    float gDenoiseSpatialSigma;
    float gDenoiseDepthSigmaMeters;
    float gDenoiseNormalPower;
    float gDenoisePad1;
}

StructuredBuffer<float4> gTemporalIndirectDiffuse : register(t0);
StructuredBuffer<float> gTemporalAmbientOcclusion : register(t1);
StructuredBuffer<float> gTemporalShadowVisibility : register(t2);
StructuredBuffer<float> gDenoiseDepthMeters : register(t3);
StructuredBuffer<float4> gDenoiseNormals : register(t4);

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

    float4 indirectSum = 0.0F.xxxx;
    float aoSum = 0.0F;
    float shadowSum = 0.0F;
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
            indirectSum += gTemporalIndirectDiffuse[tapIndex] * weight;
            aoSum += saturate(gTemporalAmbientOcclusion[tapIndex]) * weight;
            shadowSum += saturate(gTemporalShadowVisibility[tapIndex]) * weight;
            weightSum += weight;
        }
    }

    float inverseWeight = 1.0F / max(weightSum, 1.0e-6F);
    gFilteredIndirectDiffuse[index] = indirectSum * inverseWeight;
    gFilteredAmbientOcclusion[index] = aoSum * inverseWeight;
    gFilteredShadowVisibility[index] = shadowSum * inverseWeight;
}
