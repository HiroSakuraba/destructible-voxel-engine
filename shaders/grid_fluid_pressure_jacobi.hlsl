#include "common/grid_fluid.hlsli"

StructuredBuffer<float> gGridFluidPressureRead : register(t30);
StructuredBuffer<float> gGridFluidDivergence : register(t31);
StructuredBuffer<uint> gGridFluidCellTypes : register(t32);
RWStructuredBuffer<float> gGridFluidPressureWrite : register(u20);

bool is_fluid(int3 coordinate)
{
    if (any(coordinate < 0) || any(coordinate >= int3(gGridFluidDimensions.xyz))) return false;
    return gGridFluidCellTypes[grid_fluid_cell_index(uint3(coordinate))] == 0u;
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!grid_fluid_inside_cell(dispatchThreadId)) return;
    const uint index = grid_fluid_cell_index(dispatchThreadId);
    if (gGridFluidCellTypes[index] != 0u)
    {
        gGridFluidPressureWrite[index] = 0.0;
        return;
    }
    const int3 coordinate = int3(dispatchThreadId);
    const int3 offsets[6] = {
        int3(-1, 0, 0), int3(1, 0, 0), int3(0, -1, 0),
        int3(0, 1, 0), int3(0, 0, -1), int3(0, 0, 1)
    };
    float sum = 0.0;
    float diagonal = 1.0e-6;
    [unroll]
    for (uint neighbor = 0u; neighbor < 6u; ++neighbor)
    {
        const int3 other = coordinate + offsets[neighbor];
        if (!is_fluid(other)) continue;
        sum += gGridFluidPressureRead[grid_fluid_cell_index(uint3(other))];
        diagonal += 1.0;
    }
    const float rhs = -gGridFluidDivergence[index] *
                      gGridFluidStep.x * gGridFluidStep.x /
                      max(gGridFluidStep.y, 1.0e-6);
    gGridFluidPressureWrite[index] = (sum + rhs) / diagonal;
}
