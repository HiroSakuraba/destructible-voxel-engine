#ifndef DVE_SCENE_VOXEL_TRACE_HLSLI
#define DVE_SCENE_VOXEL_TRACE_HLSLI
// Ray traversal of the scene voxel grid (dve::render::SceneVoxelGrid): a 3D DDA over byte cells
// (0 = air, 1..255 = palette slot + 1). Needs player_screen_geometry.hlsli first (gGridOrigin /
// gGridSize). Distances are metres along the (unit) ray direction.
DVE_LIVE_BIND(2,0) StructuredBuffer<uint> gGridCells : register(t2);     // four byte cells per word
DVE_LIVE_BIND(3,0) StructuredBuffer<float4> gGridPalette : register(t3); // 2 x float4 per slot: albedo.xyz+metallic, emissive.xyz+0

uint GridCell(int3 c) {
    const uint3 size = (uint3)gGridSize.xyz;
    if (any(c < 0) || any((uint3)c >= size)) return 0u;
    const uint index = ((uint)c.z * size.y + (uint)c.y) * size.x + (uint)c.x;
    return (gGridCells[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu;
}

// Occupancy of 8x8x8-cell blocks, one byte per block, stored after the fine cells in the same
// buffer (word offset gGridSize.w): lets a ray cross empty space a block at a time.
static const int kCoarseBlock = 8;

bool CoarseBlockEmpty(int3 c) {
    const uint3 size = (uint3)gGridSize.xyz;
    const uint3 coarseSize = (size + (uint)(kCoarseBlock - 1)) / (uint)kCoarseBlock;
    const uint3 b = (uint3)(c / kCoarseBlock);
    const uint index = (b.z * coarseSize.y + b.y) * coarseSize.x + b.x;
    return ((gGridCells[(uint)gGridSize.w + (index >> 2u)] >> ((index & 3u) * 8u)) & 0xFFu) == 0u;
}

struct GridHit {
    float t;        // distance to the cell entered
    uint cell;      // palette slot + 1
    float3 normal;  // face the ray entered through, facing the ray
};

bool TraceSceneGrid(float3 origin, float3 dir, float maxDistance, out GridHit hit) {
    hit.t = 0.0F; hit.cell = 0u; hit.normal = -dir;
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
    const float tNear = max(max(lo.x, lo.y), lo.z);
    const float tEnter = max(tNear, 0.0F);
    const float tExit = min(min(min(hi.x, hi.y), hi.z), maxDistance);
    if (tEnter >= tExit) return false;

    // The face the ray first crosses when it enters the grid from outside.
    int entryAxis = -1;
    if (tNear > 0.0F) entryAxis = (lo.x >= lo.y && lo.x >= lo.z) ? 0 : (lo.y >= lo.z ? 1 : 2);
    const float3 start = origin + dir * (tEnter + 1.0e-4F * cell);
    int3 c = (int3)floor((start - gmin) / cell);
    c = clamp(c, int3(0, 0, 0), (int3)gGridSize.xyz - 1);
    const int3 stepDir = int3(dir.x >= 0.0F ? 1 : -1, dir.y >= 0.0F ? 1 : -1, dir.z >= 0.0F ? 1 : -1);
    const float3 boundary = gmin + (float3(c) + float3(stepDir.x > 0 ? 1.0F : 0.0F, stepDir.y > 0 ? 1.0F : 0.0F,
                                                        stepDir.z > 0 ? 1.0F : 0.0F)) * cell;
    float3 tMax = (boundary - origin) * inv;
    const float3 tDelta = abs(inv) * cell;
    float tEntry = tEnter;
    int axisCrossed = entryAxis;
    [loop] for (int i = 0; i < 384; ++i) {
        if (gGridSize.w > 0.0F && CoarseBlockEmpty(c)) {
            // Empty block: jump to where the ray leaves it, then continue stepping from there.
            const int3 blockLow = (c / kCoarseBlock) * kCoarseBlock;
            const float3 edge = gmin + (float3(blockLow) + float3(stepDir.x > 0 ? kCoarseBlock : 0, stepDir.y > 0 ? kCoarseBlock : 0,
                                                                  stepDir.z > 0 ? kCoarseBlock : 0)) * cell;
            const float3 tBlock = (edge - origin) * inv;
            float tOut = tBlock.x;
            int axis = 0;
            if (tBlock.y < tOut) { tOut = tBlock.y; axis = 1; }
            if (tBlock.z < tOut) { tOut = tBlock.z; axis = 2; }
            if (tOut >= tExit) break;
            tEntry = tOut;
            axisCrossed = axis;
            const float3 skipPoint = origin + dir * (tOut + 1.0e-4F * cell);
            c = clamp((int3)floor((skipPoint - gmin) / cell), int3(0, 0, 0), (int3)gGridSize.xyz - 1);
            const float3 nextBoundary = gmin + (float3(c) + float3(stepDir.x > 0 ? 1.0F : 0.0F, stepDir.y > 0 ? 1.0F : 0.0F,
                                                                  stepDir.z > 0 ? 1.0F : 0.0F)) * cell;
            tMax = (nextBoundary - origin) * inv;
            continue;
        }
        const uint value = GridCell(c);
        if (value != 0u) {
            hit.t = tEntry;
            hit.cell = value;
            if (axisCrossed == 0) hit.normal = float3(-(float)stepDir.x, 0.0F, 0.0F);
            else if (axisCrossed == 1) hit.normal = float3(0.0F, -(float)stepDir.y, 0.0F);
            else if (axisCrossed == 2) hit.normal = float3(0.0F, 0.0F, -(float)stepDir.z);
            return true;
        }
        float next = tMax.x;
        int axis = 0;
        if (tMax.y < next) { next = tMax.y; axis = 1; }
        if (tMax.z < next) { next = tMax.z; axis = 2; }
        if (next >= tExit) break;
        tEntry = next;
        axisCrossed = axis;
        if (axis == 0) { c.x += stepDir.x; tMax.x += tDelta.x; }
        else if (axis == 1) { c.y += stepDir.y; tMax.y += tDelta.y; }
        else { c.z += stepDir.z; tMax.z += tDelta.z; }
    }
    return false;
}
#endif
