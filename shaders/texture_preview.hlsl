// Unlit, semantic-aware texture inspection. This is deliberately isolated from scene lighting,
// fog, shadows, bloom, and material BRDF evaluation so source texels remain diagnostically useful.
cbuffer TexturePreviewConstants : register(b0) {
    uint2 gOutputSize;
    uint2 gTextureSize;
    uint gPreviewMode;       // 0 color, 1 linear data, 2 tangent normal, 3 alpha, 4 channel, 5 HDR
    uint gChannel;
    float gExposure;
    float gCheckerScale;
    float gMipLevel;
    uint gFalseColor;
    uint gArrayLayer;
    uint gNearestFiltering; // sampler selection is pipeline-owned; retained for UI/telemetry parity
    float2 gUvScale;
    float2 gUvOffset;
};

Texture2DArray<float4> gPreviewTexture : register(t0);
SamplerState gPreviewSampler : register(s0);
RWTexture2D<float4> gPreviewOutput : register(u0);

float3 linear_to_srgb(float3 value) {
    value = max(value, 0.0.xxx);
    float3 low = value * 12.92;
    float3 high = 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(value, 0.0031308.xxx));
}

float3 false_color(float value) {
    value = saturate(value);
    return saturate(float3(1.5 - abs(4.0 * value - 3.0),
                           1.5 - abs(4.0 * value - 2.0),
                           1.5 - abs(4.0 * value - 1.0)));
}

float3 checker(uint2 pixel) {
    const uint scale = max(2U, (uint)gCheckerScale);
    const bool light = ((pixel.x / scale) + (pixel.y / scale)) % 2U == 0U;
    return light ? 0.45.xxx : 0.18.xxx;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= gOutputSize)) return;
    const float2 viewportUv = (float2(pixel) + 0.5) / float2(max(gOutputSize, uint2(1U, 1U)));
    const float2 uv = viewportUv * gUvScale + gUvOffset;
    const float4 sampleValue = gPreviewTexture.SampleLevel(
        gPreviewSampler, float3(uv, (float)gArrayLayer), max(gMipLevel, 0.0));
    float4 outputValue = sampleValue;

    if (gPreviewMode == 0U) {
        outputValue.rgb = linear_to_srgb(sampleValue.rgb);
    } else if (gPreviewMode == 1U) {
        outputValue.rgb = gFalseColor != 0U ? false_color(dot(sampleValue.rgb, 1.0 / 3.0)) : sampleValue.rgb;
    } else if (gPreviewMode == 2U) {
        float3 normal = normalize(sampleValue.xyz * 2.0 - 1.0);
        outputValue = float4(normal * 0.5 + 0.5, 1.0);
    } else if (gPreviewMode == 3U) {
        const float3 background = checker(pixel);
        outputValue = float4(lerp(background, linear_to_srgb(sampleValue.rgb), sampleValue.a), 1.0);
    } else if (gPreviewMode == 4U) {
        const float channel = sampleValue[min(gChannel, 3U)];
        outputValue = float4(gFalseColor != 0U ? false_color(channel) : channel.xxx, 1.0);
    } else {
        outputValue = float4(linear_to_srgb(sampleValue.rgb * max(gExposure, 0.0)), 1.0);
    }
    gPreviewOutput[pixel] = outputValue;
}
