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

// Bounded by both a max distance and a max step count (voxels can be much smaller than the
// scatter distances involved, so distance alone could still mean a very long loop for a
// finely-voxelized object; the step cap is the real safety valve).
float MarchSubsurfaceThicknessVoxels(
    float3 entryPoint, float3 direction, uint entryMaterial,
    float maxDistanceVoxels, uint maxSteps) {
    float3 dir = normalize(direction);
    if (!(maxDistanceVoxels > 0.0F)) return 0.0F;

    int3 voxel = int3(floor(entryPoint));
    int3 step = int3(
        dir.x > 0.0F ? 1 : (dir.x < 0.0F ? -1 : 0),
        dir.y > 0.0F ? 1 : (dir.y < 0.0F ? -1 : 0),
        dir.z > 0.0F ? 1 : (dir.z < 0.0F ? -1 : 0));
    float3 tMax = float3(
        NextVoxelBoundary(entryPoint.x, dir.x, voxel.x),
        NextVoxelBoundary(entryPoint.y, dir.y, voxel.y),
        NextVoxelBoundary(entryPoint.z, dir.z, voxel.z));
    float3 tDelta = float3(
        dir.x == 0.0F ? 3.402823466e+38F : abs(1.0F / dir.x),
        dir.y == 0.0F ? 3.402823466e+38F : abs(1.0F / dir.y),
        dir.z == 0.0F ? 3.402823466e+38F : abs(1.0F / dir.z));

    float parameter = 0.0F;
    [loop] for (uint i = 0; i < maxSteps; ++i) {
        if (parameter > maxDistanceVoxels) return maxDistanceVoxels;

        int3 brickKey = BrickOfVoxel(voxel);
        uint slot = BrickSlot(brickKey);
        uint material = 0u;
        if (slot != gInvalidSlot && BrickOccupied(gBricks[slot])) {
            int3 local = LocalOfVoxel(voxel, brickKey);
            uint voxelIndex = uint(local.x + 8 * local.y + 64 * local.z);
            material = DecodeMaterial(gBricks[slot], voxelIndex);
        }
        // Stops on empty space *or* any different material, unlike TraceVoxelRay's
        // ignoreMaterial (which only treats one specific id as transparent and would sail
        // straight past a different solid material behind this one).
        if (material != entryMaterial) return parameter;

        if (tMax.x <= tMax.y && tMax.x <= tMax.z) { voxel.x += step.x; parameter = tMax.x; tMax.x += tDelta.x; }
        else if (tMax.y <= tMax.z) { voxel.y += step.y; parameter = tMax.y; tMax.y += tDelta.y; }
        else { voxel.z += step.z; parameter = tMax.z; tMax.z += tDelta.z; }
    }
    return maxDistanceVoxels; // never exited within the step budget: treat as fully thick
}

// A cheap, single-scattering-ish subsurface term (in the spirit of Christensen-Burley's
// normalized diffusion, simplified to one exponential rather than a sum of two): light
// reaching the far side of a thin section falls off exponentially with thickness relative to
// the material's own scatter distance, and "wraps" around the surface using a softened NdotL
// (dot(-normal, ...) rather than dot(normal, ...)) so thin, backlit geometry gets the
// characteristic subsurface glow instead of a hard shadow terminator.
float3 ShadeSubsurfaceTransmission(
    float thicknessMeters, float scatterDistanceMeters, float3 subsurfaceColor,
    float3 normal, float3 lightDirection, float3 lightRadiance) {
    if (!(scatterDistanceMeters > 0.0F)) return float3(0.0F, 0.0F, 0.0F);
    float attenuation = exp(-thicknessMeters / max(scatterDistanceMeters, 1.0e-4F));
    float wrappedNdotL = saturate((dot(-normal, lightDirection) + 0.5F) / 1.5F);
    return subsurfaceColor * lightRadiance * attenuation * wrappedNdotL;
}

#endif // DVE_SUBSURFACE_HLSLI
