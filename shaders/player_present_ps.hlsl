// Player present pass: linear HDR scene colour -> exposure/tint -> tonemap -> sRGB, written to
// a UNORM swapchain (or offscreen readback) image. Same curves and order as tonemap.hlsl, which
// does this for the compute path; the operator ids are TonemapOperator in render_environment.hpp.
#include "common/live_bindings.hlsli"
#include "common/tonemap_curves.hlsli"

DVE_LIVE_IMAGE(0,0) Texture2D<float4> gSceneColor : register(t0);
DVE_LIVE_IMAGE(0,0) SamplerState gSceneSampler : register(s0);
DVE_LIVE_IMAGE(2,0) Texture2D<float> gSceneDepth : register(t2);
DVE_LIVE_IMAGE(2,0) SamplerState gDepthSampler : register(s2);
DVE_LIVE_BIND(1,0) cbuffer PlayerPresentConstants : register(b0) {
    float4 gTintExposure;   // rgb global tint, w linear exposure multiplier
    uint4 gPresentOptions;  // x tonemap operator, y background: 0 none, 1 flat sky, 2 sky gradient
    float4 gSkyColor;       // RenderEnvironment::skyColor (linear radiance)
};

// Where nothing was drawn (depth still at the clear value) the background is the CPU player's
// (CpuPlayerRenderer::clear_background): skyColor, or its screen-space zenith gradient.
float3 Background(float2 uv) {
    if (gPresentOptions.y == 2u) {
        const float zenith = 1.6F - 0.9F * uv.y;
        return float3(gSkyColor.r * zenith, gSkyColor.g * zenith, gSkyColor.b * (zenith + 0.15F));
    }
    return gSkyColor.rgb;
}

static const uint kPresentTonemapACES = 0u;
static const uint kPresentTonemapReinhard = 1u;
static const uint kPresentTonemapAgX = 3u;
static const uint kPresentTonemapPBRNeutral = 4u;

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float3 radiance = gSceneColor.SampleLevel(gSceneSampler, uv, 0.0F).rgb;
    if (gPresentOptions.y != 0u && gSceneDepth.SampleLevel(gDepthSampler, uv, 0.0F) >= 1.0F)
        radiance = Background(uv);
    const float3 color = max(radiance, 0.0F.xxx) * gTintExposure.rgb * gTintExposure.w;
    float3 mapped;
    if (gPresentOptions.x == kPresentTonemapACES) mapped = TonemapACES(color);
    else if (gPresentOptions.x == kPresentTonemapReinhard) mapped = saturate(TonemapReinhard(color));
    else if (gPresentOptions.x == kPresentTonemapAgX) mapped = saturate(TonemapAgX(color));
    else if (gPresentOptions.x == kPresentTonemapPBRNeutral) mapped = saturate(TonemapPBRNeutral(color));
    else mapped = saturate(color);
    return float4(LinearToSrgb(mapped), 1.0F);
}
