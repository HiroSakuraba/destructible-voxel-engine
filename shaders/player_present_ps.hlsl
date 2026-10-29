// Player present pass: linear HDR scene colour -> exposure/tint -> tonemap -> sRGB, written to
// a UNORM swapchain (or offscreen readback) image. Same curves and order as tonemap.hlsl, which
// does this for the compute path; the operator ids are TonemapOperator in render_environment.hpp.
#include "common/live_bindings.hlsli"
#include "common/tonemap_curves.hlsli"

DVE_LIVE_IMAGE(0,0) Texture2D<float4> gSceneColor : register(t0);
DVE_LIVE_IMAGE(0,0) SamplerState gSceneSampler : register(s0);
DVE_LIVE_IMAGE(2,0) Texture2D<float> gSceneDepth : register(t2);
DVE_LIVE_IMAGE(2,0) SamplerState gDepthSampler : register(s2);
DVE_LIVE_IMAGE(3,0) Texture2D<float4> gSkyBackground : register(t3);
DVE_LIVE_IMAGE(3,0) SamplerState gSkyBackgroundSampler : register(s3);
DVE_LIVE_IMAGE(4,0) Texture2D<float4> gAerialTransmittance : register(t4);
DVE_LIVE_IMAGE(4,0) SamplerState gAerialTransmittanceSampler : register(s4);
DVE_LIVE_IMAGE(5,0) Texture2D<float4> gAerialInscatter : register(t5);
DVE_LIVE_IMAGE(5,0) SamplerState gAerialInscatterSampler : register(s5);
DVE_LIVE_BIND(1,0) cbuffer PlayerPresentConstants : register(b0) {
    float4 gTintExposure;   // rgb global tint, w linear exposure multiplier
    uint4 gPresentOptions;  // x tonemap operator, y background: 0 none, 1 flat sky, 2 sky gradient, 3 physical sky
    float4 gSkyColor;       // RenderEnvironment::skyColor (linear radiance)
    float4x4 gInverseViewProjection;
    float4 gCameraPosition;
    float4 gSunDirection;   // xyz toward the sun, w cos of the sun disc's angular radius
    float4 gSunDisc;        // rgb radiance added inside the disc
    float4 gAerial;         // x 1: aerial perspective on, y table distance range m, z distance scale, w sun-azimuth cos
    float4 gAerialSun;      // rgb sun colour x intensity x pi, w sun-azimuth sin
};

// Aerial perspective: the air between the camera and a drawn pixel. Tables built on the CPU from
// the atmosphere model (AtmosphereModel::segment): transmittance and in-scattered light by view
// direction (in the sun's frame: height, azimuth from the sun) and distance. Layout: 9 azimuth
// slices of 16 heights across, distance (sqrt-spaced, 24 steps) down; node-centred.
float3 AerialLookup(Texture2D<float4> table, SamplerState tableSampler, float3 direction, float distanceMeters) {
    const float pi = 3.14159265358979F;
    const float3 d = float3(direction.x * gAerial.w + direction.z * gAerialSun.w, direction.y,
                            -direction.x * gAerialSun.w + direction.z * gAerial.w);
    const float zenith = saturate(0.5F + 0.5F * d.y);
    const float azimuth = atan2(abs(d.z), d.x) / pi * 8.0F;
    const float slice0 = floor(min(azimuth, 7.999F));
    const float blend = azimuth - slice0;
    const float v = (sqrt(saturate(distanceMeters * gAerial.z / gAerial.y)) * 23.0F + 0.5F) / 24.0F;
    const float u0 = (slice0 * 16.0F + zenith * 15.0F + 0.5F) / 144.0F;
    const float u1 = ((slice0 + 1.0F) * 16.0F + zenith * 15.0F + 0.5F) / 144.0F;
    return lerp(table.SampleLevel(tableSampler, float2(u0, v), 0.0F).rgb,
                table.SampleLevel(tableSampler, float2(u1, v), 0.0F).rgb, blend);
}

float3 ApplyAerialPerspective(float3 radiance, float2 uv, float depth) {
    const float4 world4 = mul(gInverseViewProjection, float4(uv.x * 2.0F - 1.0F, uv.y * 2.0F - 1.0F, depth, 1.0F));
    const float3 offset = world4.xyz / world4.w - gCameraPosition.xyz;
    const float distanceMeters = length(offset);
    if (distanceMeters < 1.0e-3F) return radiance;
    const float3 direction = offset / distanceMeters;
    const float3 transmittance = AerialLookup(gAerialTransmittance, gAerialTransmittanceSampler, direction, distanceMeters);
    const float3 inscattered = AerialLookup(gAerialInscatter, gAerialInscatterSampler, direction, distanceMeters);
    return radiance * transmittance + inscattered * gAerialSun.rgb;
}

// Where nothing was drawn (depth still at the clear value) the background is the CPU player's
// (CpuPlayerRenderer::clear_background): skyColor, or its screen-space zenith gradient.
float3 PhysicalBackground(float2 uv) {
    const float4 world = mul(gInverseViewProjection, float4(uv.x * 2.0F - 1.0F, uv.y * 2.0F - 1.0F, 1.0F, 1.0F));
    const float3 direction = normalize(world.xyz / world.w - gCameraPosition.xyz);
    const float pi = 3.14159265358979F;
    const float2 skyUv = float2(atan2(direction.z, direction.x) * (0.5F / pi) + 0.5F,
                                acos(clamp(direction.y, -1.0F, 1.0F)) / pi);
    float3 sky = gSkyBackground.SampleLevel(gSkyBackgroundSampler, skyUv, 0.0F).rgb;
    if (dot(direction, gSunDirection.xyz) >= gSunDirection.w) sky += gSunDisc.rgb;
    return sky;
}

float3 Background(float2 uv) {
    if (gPresentOptions.y == 3u) return PhysicalBackground(uv);
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
    const float depth = gSceneDepth.SampleLevel(gDepthSampler, uv, 0.0F);
    if (gPresentOptions.y != 0u && depth >= 1.0F)
        radiance = Background(uv);
    else if (gAerial.x > 0.5F && depth < 1.0F)
        radiance = ApplyAerialPerspective(radiance, uv, depth);
    const float3 color = max(radiance, 0.0F.xxx) * gTintExposure.rgb * gTintExposure.w;
    float3 mapped;
    if (gPresentOptions.x == kPresentTonemapACES) mapped = TonemapACES(color);
    else if (gPresentOptions.x == kPresentTonemapReinhard) mapped = saturate(TonemapReinhard(color));
    else if (gPresentOptions.x == kPresentTonemapAgX) mapped = saturate(TonemapAgX(color));
    else if (gPresentOptions.x == kPresentTonemapPBRNeutral) mapped = saturate(TonemapPBRNeutral(color));
    else mapped = saturate(color);
    return float4(LinearToSrgb(mapped), 1.0F);
}
