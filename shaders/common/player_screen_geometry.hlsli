#ifndef DVE_PLAYER_SCREEN_GEOMETRY_HLSLI
#define DVE_PLAYER_SCREEN_GEOMETRY_HLSLI
// Depth-buffer helpers shared by the player's ambient occlusion passes: world position of a
// pixel and the surface normal rebuilt from neighbouring depths. Declares the AO bindings
// (set 0: 0 depth, 1 constants, 2 scene voxel grid cells, 3 grid palette) so both passes read one layout.
#include "live_bindings.hlsli"

DVE_LIVE_IMAGE(0,0) Texture2D<float> gDepth : register(t0);
DVE_LIVE_IMAGE(0,0) SamplerState gDepthSampler : register(s0);
DVE_LIVE_BIND(1,0) cbuffer PlayerSsaoConstants : register(b0) {
    float4x4 gViewProjection;
    float4x4 gInverseViewProjection;
    float4 gCameraPosition;  // xyz world position
    float4 gParams;          // x sample count (1..64), y max distance m, z origin bias m, w frame seed (uint bits)
    float4 gTarget;          // xy size in pixels, zw 1 / size
    float4 gGridOrigin;      // xyz world minimum corner, w cell size m (0: no grid)
    float4 gGridSize;        // xyz cells per axis
    float4 gSunDirection;    // xyz unit vector toward the sun; w sun intensity (CPU units)
    float4 gSunColor;        // rgb sun colour; w sun-visibility ray length m
    float4 gSkyColor;        // rgb sky colour (Colours sky mode); w indirect ray length m
    float4 gGroundColor;     // rgb ground colour; w indirect sample count (1..16)
};

float3 WorldFromUv(float2 uv, float depth) {
    const float4 ndc = float4(uv.x * 2.0F - 1.0F, uv.y * 2.0F - 1.0F, depth, 1.0F);
    const float4 world = mul(gInverseViewProjection, ndc);
    return world.xyz / world.w;
}

float DepthAt(float2 uv) {
    return gDepth.SampleLevel(gDepthSampler, uv, 0.0F);
}

// World position of an integer pixel centre; ok = false off screen or on the sky.
float3 PixelPosition(int2 pixel, out bool ok) {
    const int2 size = int2(gTarget.xy);
    ok = all(pixel >= 0) && all(pixel < size);
    if (!ok) return 0.0F.xxx;
    const float2 uv = (float2(pixel) + 0.5F) * gTarget.zw;
    const float depth = DepthAt(uv);
    ok = depth < 1.0F;
    return ok ? WorldFromUv(uv, depth) : 0.0F.xxx;
}

// Position and camera-facing normal of the surface at a pixel; false on the sky or a degenerate
// neighbourhood.
bool SurfaceAtPixel(int2 pixel, out float3 centre, out float3 normal) {
    centre = 0.0F.xxx;
    normal = float3(0.0F, 1.0F, 0.0F);
    if (DepthAt((float2(pixel) + 0.5F) * gTarget.zw) >= 1.0F) return false;
    bool okCentre, okL, okR, okU, okD;
    centre = PixelPosition(pixel, okCentre);
    const float3 left = PixelPosition(pixel + int2(-1, 0), okL);
    const float3 right = PixelPosition(pixel + int2(1, 0), okR);
    const float3 up = PixelPosition(pixel + int2(0, -1), okU);
    const float3 down = PixelPosition(pixel + int2(0, 1), okD);
    const float3 camera = gCameraPosition.xyz;
    // Per axis, difference towards the neighbour closer in depth: a face edge then uses the
    // neighbour on the same face instead of one across the silhouette.
    const float centreDistance = length(centre - camera);
    float3 dx = right - centre;
    float3 dy = down - centre;
    if (!okR || (okL && abs(length(left - camera) - centreDistance) < abs(length(right - camera) - centreDistance))) dx = centre - left;
    if (!okD || (okU && abs(length(up - camera) - centreDistance) < abs(length(down - camera) - centreDistance))) dy = centre - up;
    float3 n = cross(dx, dy);
    if (dot(n, n) < 1.0e-14F) return false;
    n = normalize(n);
    if (dot(n, camera - centre) < 0.0F) n = -n;
    normal = n;
    return true;
}
#endif
