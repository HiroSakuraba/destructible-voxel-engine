// RDV-1: occupancy-only ray query against RDV-2's toroidal brick buffer.
// Source occupancy is DVE's authoritative x + 8*y + 64*z indexing,
// *not* InstantRDV's subbrick/Morton wire format.
// Units: voxel indices. No DXR, mesh shaders or 64-bit shader integers.
// This independent kernel is opt-in and not used by production render passes yet.

// Each brick occupies exactly 64 bytes, independent of compiler array strides.
struct BbvRay {
    float3 origin;
    float maxDistance;
    float3 direction;
    uint rayId;
};
struct BbvResult {
    int3 voxel;
    uint hit;
    int3 normal;
    uint padding;
    float distance;
    uint voxelSteps;
    uint brickLookups;
    uint padding2;
};

// Explicit Vulkan bindings (set 0): b0 -> 0, t0/t1 -> 16/17, u0 -> 32. These equal DVE's
// -fvk-{b,t,u}-shift 0/16/32 convention but are spelled out so glslang (whose per-class shifts
// cannot place t-class and u-class structured buffers independently, which left gBbvBricks at
// binding 0 and gBbvResults unshifted) and DXC emit identical layouts. DXIL builds ignore them.
#if defined(__spirv__) || defined(RDV_VK_BINDINGS)
#define RDV_BIND(B) [[vk::binding(B, 0)]]
#else
#define RDV_BIND(B)
#endif
RDV_BIND(16) ByteAddressBuffer gBbvBricks : register(t0);
RDV_BIND(17) StructuredBuffer<BbvRay> gBbvRays : register(t1);
RDV_BIND(32) RWStructuredBuffer<BbvResult> gBbvResults : register(u0);
RDV_BIND(0) cbuffer BbvConstants : register(b0) {
    int3 gMinimumBrick;
    uint gRayCount;
    int3 gGridDimensions;
    uint gMaximumSteps;
    int3 gToroidalOffset;
    uint gPad0;
}

int FloorDiv8(int x) { return x >= 0 ? x / 8 : -((-x + 7) / 8); }
int Wrap(int x, int size) { return (x % size + size) % size; }

bool IsOccupied(int3 voxel, inout uint brickLookups) {
    int3 key = int3(FloorDiv8(voxel.x), FloorDiv8(voxel.y), FloorDiv8(voxel.z));
    int3 logical = key - gMinimumBrick;
    if (any(logical < 0) || any(logical >= gGridDimensions)) return false;
    int3 physical = int3(Wrap(logical.x + gToroidalOffset.x, gGridDimensions.x),
                         Wrap(logical.y + gToroidalOffset.y, gGridDimensions.y),
                         Wrap(logical.z + gToroidalOffset.z, gGridDimensions.z));
    uint index = (uint)(physical.x + gGridDimensions.x * (physical.y + gGridDimensions.y * physical.z));
    int3 local = voxel - key * 8;
    uint bit = (uint)(local.x + 8 * local.y + 64 * local.z);
    brickLookups++;
    uint word = gBbvBricks.Load(index * 64u + (bit >> 5u) * 4u);
    return (word & (1u << (bit & 31u))) != 0u;
}

float NextBoundary(float origin, float direction, int voxel) {
    if (direction > 0.0f) return ((float)voxel + 1.0f - origin) / direction;
    if (direction < 0.0f) return ((float)voxel - origin) / direction;
    return 3.402823466e38f;
}

[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= gRayCount) return;
    BbvRay ray = gBbvRays[index];
    BbvResult result = (BbvResult)0;
    float magnitude = length(ray.direction);
    if (!(magnitude > 0.0f) || !(ray.maxDistance >= 0.0f)) {
        gBbvResults[index] = result;
        return;
    }
    float3 direction = ray.direction / magnitude;
    float3 gridMin = float3(gMinimumBrick) * 8.0f;
    float3 gridMax = float3(gMinimumBrick + gGridDimensions) * 8.0f;
    float entry = 0.0f, exit = ray.maxDistance;
    int3 normal = int3(0,0,0);
    [unroll] for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0f) {
            if (ray.origin[axis] < gridMin[axis] || ray.origin[axis] >= gridMax[axis]) {
                gBbvResults[index] = result;
                return;
            }
            continue;
        }
        float t0 = (gridMin[axis] - ray.origin[axis]) / direction[axis];
        float t1 = (gridMax[axis] - ray.origin[axis]) / direction[axis];
        float nearT = min(t0, t1);
        if (nearT > entry) {
            entry = nearT;
            normal = int3(0,0,0);
            normal[axis] = direction[axis] > 0.0f ? -1 : 1;
        }
        exit = min(exit, max(t0, t1));
    }
    if (entry > exit) {
        gBbvResults[index] = result;
        return;
    }
    // Move into the entry cell by an IEEE-754 nextafter-style float increment.
    // A conservative clamp handles the high boundary for negative-going rays.
    float sampleT = entry > 0.0f ? entry + max(abs(entry) * 1.1920929e-7f, 1.0e-5f) : 0.0f;
    float3 p = clamp(ray.origin + direction * sampleT, gridMin, gridMax - 1.0e-4f);
    int3 voxel = int3(floor(p));
    float t = entry;
    const uint limit = min(gMaximumSteps, 65536u);
    [loop] for (uint iteration = 0; iteration < limit && t <= exit; ++iteration) {
        result.voxelSteps++;
        if (any(voxel < int3(gridMin)) || any(voxel >= int3(gridMax))) break;
        if (IsOccupied(voxel, result.brickLookups)) {
            result.hit = 1;
            result.voxel = voxel;
            result.normal = normal;
            result.distance = t;
            break;
        }
        float3 nextT = float3(NextBoundary(ray.origin.x, direction.x, voxel.x),
                             NextBoundary(ray.origin.y, direction.y, voxel.y),
                             NextBoundary(ray.origin.z, direction.z, voxel.z));
        int axis = nextT.x <= nextT.y && nextT.x <= nextT.z ? 0 :
                   nextT.y <= nextT.z ? 1 : 2;
        float next = nextT[axis];
        if (next > exit || next + 1.0e-5f < t) break;
        t = max(t, next);
        int delta = direction[axis] > 0.0f ? 1 : -1;
        voxel[axis] += delta;
        normal = int3(0,0,0);
        normal[axis] = -delta;
    }
    gBbvResults[index] = result;
}
