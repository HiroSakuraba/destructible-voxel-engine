#ifndef DVE_PLAYER_GI_COMMON_HLSLI
#define DVE_PLAYER_GI_COMMON_HLSLI
// Shared by the player's traced indirect-light passes (one-bounce and radiance cascades):
// the sky/ground colour blend and the one-bounce radiance of a voxel hit.
#include "player_screen_geometry.hlsli"
#include "scene_voxel_trace.hlsli"

static const float kPi = 3.14159265358979F;

// Equirectangular sky tables, written by the CPU from the engine's sky model (physical mode only):
// radiance by view direction and irradiance by surface normal, in the CPU renderers' convention.
DVE_LIVE_IMAGE(5,0) Texture2D<float4> gSkyRadiance : register(t5);
DVE_LIVE_IMAGE(5,0) SamplerState gSkyRadianceSampler : register(s5);
DVE_LIVE_IMAGE(6,0) Texture2D<float4> gSkyIrradiance : register(t6);
DVE_LIVE_IMAGE(6,0) SamplerState gSkyIrradianceSampler : register(s6);

float2 EquirectUv(float3 direction) {
    const float3 d = normalize(direction);
    return float2(atan2(d.z, d.x) * (0.5F / kPi) + 0.5F, acos(clamp(d.y, -1.0F, 1.0F)) / kPi);
}

float3 SkyBlend(float3 direction) {
    if (gSkyMode.x > 0.5F) return gSkyRadiance.SampleLevel(gSkyRadianceSampler, EquirectUv(direction), 0.0F).rgb;
    const float t = saturate(0.5F + 0.5F * normalize(direction).y);
    return gGroundColor.rgb * (1.0F - t) + gSkyColor.rgb * t;
}

float3 SkyIrradiance(float3 normal) {
    if (gSkyMode.x > 0.5F) return gSkyIrradiance.SampleLevel(gSkyIrradianceSampler, EquirectUv(normal), 0.0F).rgb;
    return SkyBlend(normal);
}

float3 BounceRadiance(float3 hitPoint, GridHit hit, float lift) {
    const uint slot = hit.cell - 1u;
    const float4 albedoMetallic = gGridPalette[slot * 2u];
    const float3 emissive = gGridPalette[slot * 2u + 1u].xyz;
    const float3 sun = gSunDirection.xyz;
    float3 incident = SkyIrradiance(hit.normal);
    const float ndotl = max(0.0F, dot(hit.normal, sun));
    if (gSunDirection.w > 0.0F && ndotl > 0.0F) {
        GridHit blocker;
        const bool occluded = TraceSceneGrid(hitPoint + hit.normal * lift, sun, gSunColor.w, blocker);
        if (!occluded) incident += gSunColor.rgb * (gSunDirection.w * ndotl / kPi);
    }
    return emissive + albedoMetallic.rgb * incident * (1.0F - saturate(albedoMetallic.w));
}
#endif
