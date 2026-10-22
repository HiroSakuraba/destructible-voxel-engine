#ifndef DVE_SUBSURFACE_HLSLI
#define DVE_SUBSURFACE_HLSLI

#include "brick_trace_common.hlsli"

// Genuinely voxel-specific, unlike pbr_lighting.hlsli: thickness here is measured by marching
// through the voxel grid itself (how many consecutive voxels share the entry material) rather
// than sampled from a precomputed thickness map, which is the usual mesh-renderer approach and
// isn't available here (no baked-texture pipeline exists).
//
// This does not reuse TraceVoxelRay: that function's stopping condition is "any occupied,
// non-ignored voxel," which is the wrong condition here (marching *through* the entry
// material to find where it *stops* being that material needs to keep going through occupied
// voxels of the same material and stop on anything else, including empty space). It does
// reuse TraceVoxelRay's own low-level per-voxel primitives (BrickOfVoxel, BrickSlot,
// DecodeMaterial, NextVoxelBoundary) from brick_trace_common.hlsli, so the two implementations
// share everything except the one line that actually differs.

// Where sunlight enters a Subsurface material, seen from a point on its shaded side (CPU
// reference: march_voxel_subsurface in src/render/subsurface.cpp). Starting just inside the
// entry voxel, march toward the sun through voxels of the same material until the material
// changes. If what lies beyond is empty space, sunlight enters there:
//   exitPoint       where the march left the material
//   exitNormal      the voxel face crossed, outward (toward the sun side)
//   thicknessVoxels distance from the start to the plane of that face. For a slab this is the
//                   slab's thickness at any sun angle (the slant is in dot(exitNormal, sun)).
// Bounded by both a max distance and a max step count (the step cap is the safety valve).
struct SubsurfaceMarch {
    bool exited;
    float3 exitPoint;
    float3 exitNormal;
    float thicknessVoxels;
};

SubsurfaceMarch MarchSubsurfaceThicknessVoxels(
    float3 startPoint, float3 towardSun, uint entryMaterial,
    float maxDistanceVoxels, uint maxSteps) {
    SubsurfaceMarch march;
    march.exited = false;
    march.exitPoint = startPoint;
    march.exitNormal = float3(0.0F, 1.0F, 0.0F);
    march.thicknessVoxels = 0.0F;
    float3 dir = normalize(towardSun);
    if (!(maxDistanceVoxels > 0.0F)) return march;

    int3 voxel = int3(floor(startPoint));
    int3 step = int3(
        dir.x > 0.0F ? 1 : (dir.x < 0.0F ? -1 : 0),
        dir.y > 0.0F ? 1 : (dir.y < 0.0F ? -1 : 0),
        dir.z > 0.0F ? 1 : (dir.z < 0.0F ? -1 : 0));
    float3 tMax = float3(
        NextVoxelBoundary(startPoint.x, dir.x, voxel.x),
        NextVoxelBoundary(startPoint.y, dir.y, voxel.y),
        NextVoxelBoundary(startPoint.z, dir.z, voxel.z));
    float3 tDelta = float3(
        dir.x == 0.0F ? 3.402823466e+38F : abs(1.0F / dir.x),
        dir.y == 0.0F ? 3.402823466e+38F : abs(1.0F / dir.y),
        dir.z == 0.0F ? 3.402823466e+38F : abs(1.0F / dir.z));

    [loop] for (uint i = 0; i < maxSteps; ++i) {
        float parameter;
        int3 face = int3(0, 0, 0);
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) { voxel.x += step.x; parameter = tMax.x; tMax.x += tDelta.x; face.x = step.x; }
        else if (tMax.y <= tMax.z) { voxel.y += step.y; parameter = tMax.y; tMax.y += tDelta.y; face.y = step.y; }
        else { voxel.z += step.z; parameter = tMax.z; tMax.z += tDelta.z; face.z = step.z; }
        if (parameter > maxDistanceVoxels) return march;   // too thick: no light gets through

        int3 brickKey = BrickOfVoxel(voxel);
        uint slot = BrickSlot(brickKey);
        uint material = 0u;
        if (slot != gInvalidSlot && BrickOccupied(gBricks[slot])) {
            int3 local = LocalOfVoxel(voxel, brickKey);
            uint voxelIndex = uint(local.x + 8 * local.y + 64 * local.z);
            material = DecodeMaterial(gBricks[slot], voxelIndex);
        }
        // Same material: still inside. Anything else ends the march; only empty space lets the
        // sun in (another solid material behind this one blocks it).
        if (material == entryMaterial) continue;
        march.exited = material == 0u;
        march.exitPoint = startPoint + dir * parameter;
        march.exitNormal = float3(face);
        march.thicknessVoxels = clamp(dot(march.exitPoint - startPoint, march.exitNormal), 0.0F, parameter);
        return march;
    }
    return march;   // never left within the step budget: treat as fully thick
}

// Burley's normalized-diffusion transmittance through a thickness d, for scatter distance s:
// the diffusion profile integrated over the plane at depth d (Burley 2015). T(0) = 1.
float BurleyTransmittance(float thicknessMeters, float scatterDistanceMeters) {
    if (!(scatterDistanceMeters > 0.0F)) return 0.0F;
    float x = max(thicknessMeters, 0.0F) / scatterDistanceMeters;
    return 0.25F * exp(-x) + 0.75F * exp(-x / 3.0F);
}

// Sunlight transmitted to the shaded side: the subsurface colour (the albedo of this light),
// the sun arriving at the sunlit face (its cosine there, and that face's own sun visibility in
// lightRadiance), and the Burley transmittance through the thickness.
float3 ShadeSubsurfaceTransmission(
    float thicknessMeters, float scatterDistanceMeters, float3 subsurfaceColor,
    float3 exitNormal, float3 lightDirection, float3 lightRadiance) {
    float cosEntry = saturate(dot(exitNormal, lightDirection));
    return subsurfaceColor * lightRadiance * cosEntry *
           BurleyTransmittance(thicknessMeters, scatterDistanceMeters);
}

#endif // DVE_SUBSURFACE_HLSLI
