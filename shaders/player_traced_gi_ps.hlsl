// Voxel one-bounce global illumination for the Vulkan player renderer.
//
// The GPU twin of the CPU reference's GlobalIlluminationMode::VoxelOneBounce
// (voxel_reference_renderer.cpp global_illumination + radiance_cascades_spwi.cpp
// bounce_hit_radiance + ray_lighting_reference.cpp one_bounce_diffuse_radiance):
//   * each pixel sends N cosine-weighted rays from its surface into the scene voxel grid;
//   * a ray that leaves the scene (or the ray length) sees the sky along its direction;
//   * a ray that hits a voxel sees that voxel's one-bounce radiance:
//       emissive + albedo (1 - metallic) (sky irradiance at the hit normal
//                                          + sun colour x intensity x max(0, n.l) x visibility / pi)
//     where visibility is one ray from the hit toward the sun through the same grid;
//   * the pixel's indirect light is the mean over its rays.
// The result (radiance, what a white diffuse surface would draw, intensity not applied) goes to
// the live material shader, which uses it in place of the environment's diffuse integral.
// Runs at half resolution with a few rays per texel; player_gi_temporal_ps accumulates frames and
// player_gi_upsample_ps filters the result up to full resolution.
#include "common/player_screen_geometry.hlsli"
#include "common/random_sampling.hlsli"
#include "common/player_gi_common.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    // Half resolution: each texel traces one pixel of its 2x2 block, a different one each frame.
    const int2 pixel = HalfTexelPixel(int2(position.xy));
    float3 centre, normal;
    if (gGridOrigin.w <= 0.0F || !SurfaceAtPixel(pixel, centre, normal)) return float4(0.0F, 0.0F, 0.0F, 0.0F);

    const uint sampleCount = clamp((uint)gGroundColor.w, 1u, 16u);
    const float maxDistance = max(gSkyColor.w, 1.0e-3F);
    // Grid cells are as coarse as the voxels: start far enough off the surface that the cell
    // holding the surface itself is not counted as the first hit.
    const float lift = max(max(1.0e-3F, gParams.z), 1.1F * gGridOrigin.w);
    const float3 origin = centre + normal * lift;
    const uint pixelIndex = (uint)position.y * (uint)gGiTemporal.z + (uint)position.x;
    const uint seed = asuint(gPreviousCamera.w) * 2654435761u;
    float3 accumulated = 0.0F.xxx;
    for (uint i = 0u; i < sampleCount; ++i) {
        const float3 direction = CosineWeightedHemisphereSample(
            normal, RotatedHammersley2D(i, sampleCount, PcgHash(pixelIndex ^ seed)));
        GridHit hit;
        if (TraceSceneGrid(origin, direction, max(maxDistance - lift, 0.0F), hit))
            accumulated += BounceRadiance(origin + direction * hit.t, hit, lift);
        else
            accumulated += SkyBlend(direction);
    }
    return float4(accumulated / float(sampleCount), 1.0F);
}
