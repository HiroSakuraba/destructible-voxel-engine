// Screen-space ambient occlusion for the Vulkan player renderer.
//
// Same definition as the CPU reference (dve/render/ambient_occlusion.hpp, generate_ao_rays.hlsl,
// resolve_ao.hlsl): ray i of n leaves the surface (raised along the normal by the shadow bias)
// along CosineWeightedHemisphereSample(normal, RotatedHammersley2D(i, n, PcgHash(pixel ^ seed)));
// a hit at distance d within the maximum distance occludes (1 - d / max)^2; AO = 1 - mean.
//
// The difference is what a ray can hit: the CPU traces the voxel/polygon scene, this pass
// marches each ray through the camera's depth buffer, so it only sees geometry that is on screen
// and facing the camera. Everything else (the sky, geometry behind the visible surface, rays that
// leave the screen) counts as open.
#include "common/live_bindings.hlsli"
#include "common/random_sampling.hlsli"

DVE_LIVE_IMAGE(0,0) Texture2D<float> gDepth : register(t0);
DVE_LIVE_IMAGE(0,0) SamplerState gDepthSampler : register(s0);
DVE_LIVE_BIND(1,0) cbuffer PlayerSsaoConstants : register(b0) {
    float4x4 gViewProjection;
    float4x4 gInverseViewProjection;
    float4 gCameraPosition;  // xyz world position
    float4 gParams;          // x sample count (1..64), y max distance m, z origin bias m, w frame seed (uint bits)
    float4 gTarget;          // xy size in pixels, zw 1 / size
};

static const int kMarchSteps = 16;

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

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const uint sampleCount = clamp((uint)gParams.x, 1u, 64u);
    const float maxDistance = max(gParams.y, 1.0e-4F);
    const float centreDepth = DepthAt((float2(pixel) + 0.5F) * gTarget.zw);
    if (centreDepth >= 1.0F) return float4(1.0F, 1.0F, 1.0F, 1.0F);

    bool okCentre, okL, okR, okU, okD;
    const float3 centre = PixelPosition(pixel, okCentre);
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
    float3 normal = cross(dx, dy);
    if (dot(normal, normal) < 1.0e-14F) return float4(1.0F, 1.0F, 1.0F, 1.0F);
    normal = normalize(normal);
    if (dot(normal, camera - centre) < 0.0F) normal = -normal;

    const uint pixelIndex = (uint)pixel.y * (uint)gTarget.x + (uint)pixel.x;
    const uint seed = asuint(gParams.w);
    const float3 origin = centre + normal * max(1.0e-3F, gParams.z);
    const float jitter = Hash2(pixelIndex ^ (seed * 2654435761u)).x;
    float occlusion = 0.0F;
    for (uint i = 0u; i < sampleCount; ++i) {
        const float3 direction = CosineWeightedHemisphereSample(
            normal, RotatedHammersley2D(i, sampleCount, PcgHash(pixelIndex ^ seed)));
        [loop] for (int step = 0; step < kMarchSteps; ++step) {
            const float t = maxDistance * (float(step) + 0.25F + 0.5F * jitter) / float(kMarchSteps);
            const float3 samplePoint = origin + direction * t;
            const float4 clip = mul(gViewProjection, float4(samplePoint, 1.0F));
            if (clip.w <= 1.0e-5F) break;
            const float2 sampleUv = clip.xy / clip.w * 0.5F + 0.5F;
            if (any(sampleUv < 0.0F) || any(sampleUv > 1.0F)) break;
            const float sceneDepth = DepthAt(sampleUv);
            if (sceneDepth >= 1.0F) continue;
            const float3 scenePoint = WorldFromUv(sampleUv, sceneDepth);
            const float pointDistance = length(samplePoint - camera);
            const float gap = pointDistance - length(scenePoint - camera);
            // Behind the visible surface, but not so far behind that it is a different object.
            if (gap > 0.004F * pointDistance && gap < maxDistance) {
                const float proximity = 1.0F - saturate(t / maxDistance);
                occlusion += proximity * proximity;
                break;
            }
        }
    }
    const float ao = saturate(1.0F - occlusion / float(sampleCount));
    return float4(ao, ao, ao, 1.0F);
}
