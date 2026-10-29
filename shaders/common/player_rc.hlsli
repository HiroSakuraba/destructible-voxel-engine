#ifndef DVE_PLAYER_RC_HLSLI
#define DVE_PLAYER_RC_HLSLI
// Radiance cascades for the player's indirect light.
//
// Four cascades of screen-space probes on the depth buffer. Level i has a probe every 8 * 2^i
// pixels and a (4 * 2^i)^2 octahedral direction map per probe, so every level holds the same
// number of rays (about width/2 x height/2). Level i covers the ray interval
// [L (4^i - 1)/3, L (4^(i+1) - 1)/3) from the probe; a ray that finds nothing in its interval
// continues with the radiance of the next level up (bilateral blend of the four nearest probes).
// Texture layout of a level: texel (probe.x * dirRes + d.x, probe.y * dirRes + d.y).
#include "player_gi_common.hlsli"

DVE_LIVE_IMAGE(4,0) Texture2D<float4> gRcUpper : register(t4);
DVE_LIVE_IMAGE(4,0) SamplerState gRcUpperSampler : register(s4);

float3 OctahedralDirection(float2 uv) {
    const float2 p = uv * 2.0F - 1.0F;
    float3 v = float3(p.x, 1.0F - abs(p.x) - abs(p.y), p.y);
    if (v.y < 0.0F) {
        const float2 folded = (1.0F - abs(v.zx)) * float2(v.x >= 0.0F ? 1.0F : -1.0F, v.z >= 0.0F ? 1.0F : -1.0F);
        v.x = folded.x;
        v.z = folded.y;
    }
    return normalize(v);
}

// Solid angle of an octahedral texel relative to the mean (4 pi / texel count).
float OctahedralTexelWeight(float2 uv, float dirRes) {
    const float2 p = uv * 2.0F - 1.0F;
    const float3 v = float3(p.x, 1.0F - abs(p.x) - abs(p.y), p.y);
    const float length3 = max(length(v), 1.0e-4F);
    return 4.0F / (length3 * length3 * length3) / (dirRes * dirRes);
}

// A probe: its centre pixel, and the surface there. valid = false on the sky.
struct RcProbe {
    bool valid;
    float3 position;
    float3 normal;
};

RcProbe LoadProbe(int2 probe, float spacing) {
    RcProbe result;
    const int2 pixel = clamp(int2((float2(probe) + 0.5F) * spacing), int2(0, 0), int2(gTarget.xy) - 1);
    result.valid = SurfaceAtPixel(pixel, result.position, result.normal);
    return result;
}

// Weight of a candidate probe for a shading point: how close in plane distance and normal.
float ProbeCompatibility(float3 position, float3 normal, RcProbe probe, float sigma) {
    if (!probe.valid) return 0.0F;
    const float planeDistance = abs(dot(normal, probe.position - position));
    const float distanceTerm = exp(-(planeDistance * planeDistance) / max(sigma * sigma, 1.0e-8F));
    const float normalTerm = pow(max(dot(normal, probe.normal), 0.0F), 8.0F);
    return distanceTerm * normalTerm;
}

// World size of one pixel at a position, along the screen.
float PixelFootprint(float3 position) {
    return length(position - gCameraPosition.xyz) * gTarget.w * 1.15F;
}
#endif
