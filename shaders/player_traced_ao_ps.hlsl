// Voxel-traced ambient occlusion for the Vulkan player renderer.
//
// Same definition as player_ssao_ps.hlsl and the CPU reference (cosine hemisphere rays, a hit at
// distance d within the maximum distance occludes (1 - d / max)^2, AO = 1 - mean), but the rays
// walk the scene voxel grid (dve::render::SceneVoxelGrid) with a 3D DDA instead of marching the
// depth buffer. Occluders therefore do not have to be on screen. Anything outside the grid, and
// polygon (non-voxel) objects, count as open.
#include "common/player_screen_geometry.hlsli"
#include "common/random_sampling.hlsli"
#include "common/scene_voxel_trace.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const uint sampleCount = clamp((uint)gParams.x, 1u, 64u);
    const float maxDistance = max(gParams.y, 1.0e-4F);
    float3 centre, normal;
    if (!SurfaceAtPixel(pixel, centre, normal) || gGridOrigin.w <= 0.0F) return float4(1.0F, 1.0F, 1.0F, 1.0F);

    // Grid cells are as coarse as the voxels: start far enough off the surface that the cell
    // holding the surface itself is not counted as an occluder (the offset is part of d).
    const float lift = max(max(1.0e-3F, gParams.z), 1.1F * gGridOrigin.w);
    const float3 origin = centre + normal * lift;
    const uint pixelIndex = (uint)pixel.y * (uint)gTarget.x + (uint)pixel.x;
    const uint seed = asuint(gParams.w);
    float occlusion = 0.0F;
    for (uint i = 0u; i < sampleCount; ++i) {
        const float3 direction = CosineWeightedHemisphereSample(
            normal, RotatedHammersley2D(i, sampleCount, PcgHash(pixelIndex ^ seed)));
        GridHit hit;
        if (TraceSceneGrid(origin, direction, max(maxDistance - lift, 0.0F), hit)) {
            const float proximity = 1.0F - saturate((hit.t + lift) / maxDistance);
            occlusion += proximity * proximity;
        }
    }
    const float ao = saturate(1.0F - occlusion / float(sampleCount));
    return float4(ao, ao, ao, 1.0F);
}
