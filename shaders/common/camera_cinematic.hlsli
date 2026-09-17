#ifndef DVE_CAMERA_CINEMATIC_HLSLI
#define DVE_CAMERA_CINEMATIC_HLSLI

struct CameraGpuPacket {
    float4 view[4];
    float4 projection[4];
    float4 viewProjection[4];
    float4 cameraPosition;
    float4 viewportRect;
    float4 lens;
    float4 physicalLens0;
    float4 physicalLens1;
    float4 postProcess0;
    float4 postProcess1;
    float4 colorTint;
    float4 colorGrade0;
    float4 colorGradeLift;
    float4 colorGradeGamma;
    float4 colorGradeGain;
    float4 lensEffects0;
    float4 lensEffects1;
    float4 lensEffects2;
    float4 bokeh;
    float4 splitDiopter0;
    float4 splitDiopter1;
    float4 film0;
    float4 film1;
    float4 matteColor;
    uint outputChannelMask;
    uint cameraCutGeneration;
    uint resetTemporalHistory;
    uint reserved;
};

float Luminance(float3 color) {
    return dot(color, float3(0.2126F, 0.7152F, 0.0722F));
}

float HashNoise(uint2 pixel, uint frameIndex, uint seed) {
    uint value = pixel.x * 0x1f123bb5U + pixel.y * 0x5f356495U +
                 frameIndex * 0x9e3779b9U + seed * 0x85ebca6bU;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return float(value & 0x00ffffffU) / 16777215.0F;
}

float3 TonemapCamera(float3 color, uint curve) {
    color = max(color, 0.0F.xxx);
    if (curve == 0U) return saturate(color);
    if (curve == 1U) return color / (1.0F.xxx + color);
    const float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float FramingAspect(uint framing, float customAspect, float nativeAspect) {
    if (framing == 1U) return 1.375F;
    if (framing == 2U) return 1.43F;
    if (framing == 3U) return 1.90F;
    if (framing == 4U) return 1.85F;
    if (framing == 5U) return 2.39F;
    if (framing == 6U) return max(0.1F, customAspect);
    return nativeAspect;
}

#endif
