#ifndef DVE_BRICK_TRACE_COMMON_HLSLI
#define DVE_BRICK_TRACE_COMMON_HLSLI

// Factored out of the v1.4 primary tracer contract (previously all inline in
// primary_brickmap_trace.hlsl) so the ambient-occlusion, subsurface-thickness, and
// translucency-continuation passes can all dispatch the exact same, single traversal
// implementation instead of each carrying their own copy of the DDA. The algorithm itself is
// unchanged from the original single-file version: this is a mechanical extraction (loop body
// moved into TraceVoxelRay, everything else left exactly as it read before), not a rewrite.
//
// Shader Model 6.0 baseline: uint32 only, no DXR, no mesh shaders, no int64.

struct PackedGpuBrickRecord {
    int3 key;
    uint generation;
    uint encoding;
    uint uniformMaterial;
    uint materialOffset;
    uint materialBytes;
    uint occupancy32[16];
    uint halo32[12];
};

// origin, maxDistance, and TraceResult.distance use voxel-index units. Directions are
// dimensionless. Metre-authored lighting distances are converted in the generation shaders.
struct TraceRay {
    float3 origin;
    float maxDistance;
    float3 direction;
    uint rayId;
};

struct TraceResult {
    int3 voxel;
    uint hit;
    int3 normal;
    uint material;
    float distance;
    uint brickLookups;
    uint emptyBrickSkips;
    uint voxelSteps;
};


float3 TraceHitPosition(TraceRay ray, TraceResult result) {
    float directionLength = length(ray.direction);
    if (!(directionLength > 0.0F)) return ray.origin;
    return ray.origin + ray.direction * (result.distance / directionLength);
}

StructuredBuffer<uint> gBrickIndex : register(t0);
StructuredBuffer<PackedGpuBrickRecord> gBricks : register(t1);
ByteAddressBuffer gMaterials : register(t2);

cbuffer TraceConstants : register(b0) {
    int3 gMinBrick;
    uint gRayCount;
    int3 gBrickExtent;
    uint gInvalidSlot;
}

int FloorDiv8(int value) {
    return value >= 0 ? value / 8 : -((-value + 7) / 8);
}

int3 BrickOfVoxel(int3 voxel) {
    return int3(FloorDiv8(voxel.x), FloorDiv8(voxel.y), FloorDiv8(voxel.z));
}

int3 LocalOfVoxel(int3 voxel, int3 brick) {
    return voxel - brick * 8;
}

uint BrickSlot(int3 brick) {
    int3 local = brick - gMinBrick;
    if (any(local < 0) || any(local >= gBrickExtent)) return gInvalidSlot;
    uint index = uint(local.x + gBrickExtent.x * (local.y + gBrickExtent.y * local.z));
    return gBrickIndex[index];
}

bool BrickOccupied(PackedGpuBrickRecord brick) {
    uint combined = 0;
    [unroll] for (uint i = 0; i < 16; ++i) combined |= brick.occupancy32[i];
    return combined != 0;
}

uint LoadMaterialByte(uint address) {
    uint aligned = address & ~3u;
    uint packed = gMaterials.Load(aligned);
    return (packed >> ((address & 3u) * 8u)) & 0xFFu;
}

uint DecodeMaterial(PackedGpuBrickRecord brick, uint voxelIndex) {
    uint word = brick.occupancy32[voxelIndex >> 5u];
    if (((word >> (voxelIndex & 31u)) & 1u) == 0u) return 0u;
    if (brick.encoding == 1u || brick.encoding == 2u) return brick.uniformMaterial;
    if (brick.encoding == 3u && brick.materialBytes >= 272u) {
        uint packed = LoadMaterialByte(brick.materialOffset + (voxelIndex >> 1u));
        uint paletteIndex = (voxelIndex & 1u) == 0u ? packed & 0xFu : packed >> 4u;
        return LoadMaterialByte(brick.materialOffset + 256u + paletteIndex);
    }
    if (brick.encoding == 4u && brick.materialBytes >= 512u) {
        return LoadMaterialByte(brick.materialOffset + voxelIndex);
    }
    return 0u;
}

float NextVoxelBoundary(float origin, float direction, int voxel) {
    if (direction > 0.0) return (float(voxel + 1) - origin) / direction;
    if (direction < 0.0) return (float(voxel) - origin) / direction;
    return 3.402823466e+38F;
}

// Traces one ray through the brickmap to the first occupied voxel (material != 0), or reports
// no hit. Every one of this file's callers (primary visibility, AO, thickness probes,
// translucency continuation) uses exactly this function; none of them re-implement the DDA.
//
// `ignoreMaterial`: if non-zero, a voxel with exactly this material id is treated as *not*
// occupied and the ray continues through it. This is what the subsurface-thickness pass uses
// to march through a material's own interior without stopping on its first voxel (the entry
// point), and what translucency continuation uses to skip past a specific transparent hit
// already accounted for. Pass 0 (kAirMaterial's own id) for a normal, stop-on-any-hit trace,
// since 0 already means "empty" and can never itself be a real occupied voxel's material.
TraceResult TraceVoxelRay(TraceRay input, uint ignoreMaterial) {
    TraceResult output = (TraceResult)0;
    output.voxel = int3(0, 0, 0);
    output.normal = int3(0, 0, 0);
    output.distance = 0.0;

    float magnitude = length(input.direction);
    if (!(magnitude > 0.0) || !(input.maxDistance >= 0.0)) return output;

    float3 direction = input.direction;
    float maximumParameter = input.maxDistance / magnitude;
    int3 voxel = int3(floor(input.origin));
    int3 step = int3(
        direction.x > 0.0 ? 1 : (direction.x < 0.0 ? -1 : 0),
        direction.y > 0.0 ? 1 : (direction.y < 0.0 ? -1 : 0),
        direction.z > 0.0 ? 1 : (direction.z < 0.0 ? -1 : 0));
    float3 tMax = float3(
        NextVoxelBoundary(input.origin.x, direction.x, voxel.x),
        NextVoxelBoundary(input.origin.y, direction.y, voxel.y),
        NextVoxelBoundary(input.origin.z, direction.z, voxel.z));
    float3 tDelta = float3(
        direction.x == 0.0 ? 3.402823466e+38F : abs(1.0 / direction.x),
        direction.y == 0.0 ? 3.402823466e+38F : abs(1.0 / direction.y),
        direction.z == 0.0 ? 3.402823466e+38F : abs(1.0 / direction.z));

    float parameter = 0.0;
    int3 normal = int3(0, 0, 0);
    [loop] while (parameter <= maximumParameter) {
        int3 brickKey = BrickOfVoxel(voxel);
        output.brickLookups++;
        uint slot = BrickSlot(brickKey);
        bool occupiedBrick = slot != gInvalidSlot && BrickOccupied(gBricks[slot]);
        if (!occupiedBrick) {
            output.emptyBrickSkips++;
            int3 emptyBrick = brickKey;
            [loop] while (parameter <= maximumParameter && all(BrickOfVoxel(voxel) == emptyBrick)) {
                output.voxelSteps++;
                if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
                    voxel.x += step.x; parameter = tMax.x; tMax.x += tDelta.x; normal = int3(-step.x, 0, 0);
                } else if (tMax.y <= tMax.z) {
                    voxel.y += step.y; parameter = tMax.y; tMax.y += tDelta.y; normal = int3(0, -step.y, 0);
                } else {
                    voxel.z += step.z; parameter = tMax.z; tMax.z += tDelta.z; normal = int3(0, 0, -step.z);
                }
            }
            continue;
        }

        int3 activeBrick = brickKey;
        [loop] while (parameter <= maximumParameter && all(BrickOfVoxel(voxel) == activeBrick)) {
            int3 local = LocalOfVoxel(voxel, activeBrick);
            uint voxelIndex = uint(local.x + 8 * local.y + 64 * local.z);
            uint material = DecodeMaterial(gBricks[slot], voxelIndex);
            if (material != 0u && material != ignoreMaterial) {
                output.hit = 1u;
                output.voxel = voxel;
                output.normal = normal;
                output.material = material;
                output.distance = parameter * magnitude;
                return output;
            }
            output.voxelSteps++;
            if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
                voxel.x += step.x; parameter = tMax.x; tMax.x += tDelta.x; normal = int3(-step.x, 0, 0);
            } else if (tMax.y <= tMax.z) {
                voxel.y += step.y; parameter = tMax.y; tMax.y += tDelta.y; normal = int3(0, -step.y, 0);
            } else {
                voxel.z += step.z; parameter = tMax.z; tMax.z += tDelta.z; normal = int3(0, 0, -step.z);
            }
        }
    }
    return output;
}

TraceResult TraceVoxelRay(TraceRay input) {
    return TraceVoxelRay(input, 0u);
}

#endif // DVE_BRICK_TRACE_COMMON_HLSLI
