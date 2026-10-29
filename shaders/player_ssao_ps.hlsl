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
#include "common/player_screen_geometry.hlsli"
#include "common/random_sampling.hlsli"

static const int kMarchSteps = 16;

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const uint sampleCount = clamp((uint)gParams.x, 1u, 64u);
    const float maxDistance = max(gParams.y, 1.0e-4F);
    float3 centre, normal;
    if (!SurfaceAtPixel(pixel, centre, normal)) return float4(1.0F, 1.0F, 1.0F, 1.0F);
    const float3 camera = gCameraPosition.xyz;

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
