#ifndef DVE_PLAYER_GI_COMMON_HLSLI
#define DVE_PLAYER_GI_COMMON_HLSLI
// Shared by the player's traced indirect-light passes (one-bounce and radiance cascades):
// the sky/ground colour blend and the one-bounce radiance of a voxel hit.
#include "player_screen_geometry.hlsli"
#include "scene_voxel_trace.hlsli"

static const float kPi = 3.14159265358979F;

float3 SkyBlend(float3 direction) {
    const float t = saturate(0.5F + 0.5F * normalize(direction).y);
    return gGroundColor.rgb * (1.0F - t) + gSkyColor.rgb * t;
}

float3 BounceRadiance(float3 hitPoint, GridHit hit, float lift) {
    const uint slot = hit.cell - 1u;
    const float4 albedoMetallic = gGridPalette[slot * 2u];
    const float3 emissive = gGridPalette[slot * 2u + 1u].xyz;
    const float3 sun = gSunDirection.xyz;
    float3 incident = SkyBlend(hit.normal);   // sky_irradiance in Colours mode
    const float ndotl = max(0.0F, dot(hit.normal, sun));
    if (gSunDirection.w > 0.0F && ndotl > 0.0F) {
        GridHit blocker;
        const bool occluded = TraceSceneGrid(hitPoint + hit.normal * lift, sun, gSunColor.w, blocker);
        if (!occluded) incident += gSunColor.rgb * (gSunDirection.w * ndotl / kPi);
    }
    return emissive + albedoMetallic.rgb * incident * (1.0F - saturate(albedoMetallic.w));
}
#endif
