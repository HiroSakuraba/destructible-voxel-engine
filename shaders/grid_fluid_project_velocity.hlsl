#include "common/grid_fluid.hlsli"

StructuredBuffer<float> gGridFluidPressure : register(t30);
StructuredBuffer<uint> gGridFluidCellTypes : register(t31);
RWStructuredBuffer<float> gGridFluidVelocityComponent : register(u20);

bool is_fluid(int3 coordinate)
{
    if (any(coordinate < 0) || any(coordinate >= int3(gGridFluidDimensions.xyz))) return false;
    return gGridFluidCellTypes[grid_fluid_cell_index(uint3(coordinate))] == 0u;
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint3 dimensions = gGridFluidDimensions.xyz;
    if (gGridFluidDimensions.w == 0u) dimensions.x += 1u;
    if (gGridFluidDimensions.w == 1u) dimensions.y += 1u;
    if (gGridFluidDimensions.w == 2u) dimensions.z += 1u;
    if (any(dispatchThreadId >= dimensions)) return;
    int3 left = int3(dispatchThreadId);
    int3 right = int3(dispatchThreadId);
    if (gGridFluidDimensions.w == 0u) left.x -= 1;
    if (gGridFluidDimensions.w == 1u) left.y -= 1;
    if (gGridFluidDimensions.w == 2u) left.z -= 1;
    const uint index = dispatchThreadId.x + dimensions.x *
                       (dispatchThreadId.y + dimensions.y * dispatchThreadId.z);
    if (!is_fluid(left) || !is_fluid(right))
    {
        gGridFluidVelocityComponent[index] = 0.0;
        return;
    }
    const float pressureLeft = gGridFluidPressure[grid_fluid_cell_index(uint3(left))];
    const float pressureRight = gGridFluidPressure[grid_fluid_cell_index(uint3(right))];
    gGridFluidVelocityComponent[index] -=
        gGridFluidStep.y * (pressureRight - pressureLeft) / gGridFluidStep.x;
}
