// Voxel-traced ambient occlusion for the Vulkan player renderer.
//
// Same definition as player_ssao_ps.hlsl and the CPU reference (cosine hemisphere rays, a hit at
// distance d within the maximum distance occludes (1 - d / max)^2, AO = 1 - mean), but the rays
// walk the scene voxel grid (dve::render::SceneVoxelGrid) with a 3D DDA instead of marching the
// depth buffer. Occluders therefore do not have to be on screen. Anything outside the grid, and
// polygon (non-voxel) objects, count as open.
#include "common/player_screen_geometry.hlsli"
#include "common/random_sampling.hlsli"

DVE_LIVE_BIND(2,0) StructuredBuffer<uint> gGridCells : register(t2);   // four byte cells per word

bool SolidCell(int3 c) {
    const uint3 size = (uint3)gGridSize.xyz;
    if (any(c < 0) || any((uint3)c >= size)) return false;
    const uint index = ((uint)c.z * size.y + (uint)c.y) * size.x + (uint)c.x;
    return ((gGridCells[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu) != 0u;
}

// Distance along `dir` (metres from `origin`) to the first solid cell within maxDistance, or a
// negative number when the ray stays clear.
float TraceGrid(float3 origin, float3 dir, float maxDistance) {
    const float cell = gGridOrigin.w;
    const float3 gmin = gGridOrigin.xyz;
    const float3 gmax = gmin + gGridSize.xyz * cell;
    const float3 safeDir = float3(abs(dir.x) < 1.0e-8F ? 1.0e-8F : dir.x, abs(dir.y) < 1.0e-8F ? 1.0e-8F : dir.y,
                                  abs(dir.z) < 1.0e-8F ? 1.0e-8F : dir.z);
    const float3 inv = 1.0F / safeDir;
    const float3 t0 = (gmin - origin) * inv;
    const float3 t1 = (gmax - origin) * inv;
    const float3 lo = min(t0, t1);
    const float3 hi = max(t0, t1);
    const float tEnter = max(max(max(lo.x, lo.y), lo.z), 0.0F);
    const float tExit = min(min(min(hi.x, hi.y), hi.z), maxDistance);
    if (tEnter >= tExit) return -1.0F;

    const float3 start = origin + dir * (tEnter + 1.0e-4F * cell);
    int3 c = (int3)floor((start - gmin) / cell);
    c = clamp(c, int3(0, 0, 0), (int3)gGridSize.xyz - 1);
    const int3 stepDir = int3(dir.x >= 0.0F ? 1 : -1, dir.y >= 0.0F ? 1 : -1, dir.z >= 0.0F ? 1 : -1);
    const float3 boundary = gmin + (float3(c) + float3(stepDir.x > 0 ? 1.0F : 0.0F, stepDir.y > 0 ? 1.0F : 0.0F,
                                                        stepDir.z > 0 ? 1.0F : 0.0F)) * cell;
    float3 tMax = (boundary - origin) * inv;
    const float3 tDelta = abs(inv) * cell;
    float tEntry = tEnter;
    [loop] for (int i = 0; i < 256; ++i) {
        if (SolidCell(c)) return tEntry;
        float next = tMax.x;
        int axis = 0;
        if (tMax.y < next) { next = tMax.y; axis = 1; }
        if (tMax.z < next) { next = tMax.z; axis = 2; }
        if (next >= tExit) break;
        tEntry = next;
        if (axis == 0) { c.x += stepDir.x; tMax.x += tDelta.x; }
        else if (axis == 1) { c.y += stepDir.y; tMax.y += tDelta.y; }
        else { c.z += stepDir.z; tMax.z += tDelta.z; }
    }
    return -1.0F;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const uint sampleCount = clamp((uint)gParams.x, 1u, 64u);
    const float maxDistance = max(gParams.y, 1.0e-4F);
    float3 centre, normal;
    if (!SurfaceAtPixel(pixel, centre, normal) || gGridOrigin.w <= 0.0F) return float4(1.0F, 1.0F, 1.0F, 1.0F);

    // Grid cells are as coarse as the voxels: start far enough off the surface that the cell
    // holding the surface itself is not counted as an occluder (the offset is part of d).
    const float lift = max(max(1.0e-3F, gParams.z), 0.35F * gGridOrigin.w);
    const float3 origin = centre + normal * lift;
    const uint pixelIndex = (uint)pixel.y * (uint)gTarget.x + (uint)pixel.x;
    const uint seed = asuint(gParams.w);
    float occlusion = 0.0F;
    for (uint i = 0u; i < sampleCount; ++i) {
        const float3 direction = CosineWeightedHemisphereSample(
            normal, RotatedHammersley2D(i, sampleCount, PcgHash(pixelIndex ^ seed)));
        const float hit = TraceGrid(origin, direction, max(maxDistance - lift, 0.0F));
        if (hit >= 0.0F) {
            const float proximity = 1.0F - saturate((hit + lift) / maxDistance);
            occlusion += proximity * proximity;
        }
    }
    const float ao = saturate(1.0F - occlusion / float(sampleCount));
    return float4(ao, ao, ao, 1.0F);
}
