#include "common/grid_fluid.hlsli"

StructuredBuffer<float> gGridFluidU : register(t30);
StructuredBuffer<float> gGridFluidV : register(t31);
StructuredBuffer<float> gGridFluidW : register(t32);
StructuredBuffer<uint> gGridFluidCellTypes : register(t33);
RWStructuredBuffer<float> gGridFluidDivergence : register(u20);

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!grid_fluid_inside_cell(dispatchThreadId)) return;
    const uint index = grid_fluid_cell_index(dispatchThreadId);
    if (gGridFluidCellTypes[index] != 0u)
    {
        gGridFluidDivergence[index] = 0.0;
        return;
    }
    const float value =
        gGridFluidU[grid_fluid_u_index(dispatchThreadId + uint3(1u, 0u, 0u))] -
        gGridFluidU[grid_fluid_u_index(dispatchThreadId)] +
        gGridFluidV[grid_fluid_v_index(dispatchThreadId + uint3(0u, 1u, 0u))] -
        gGridFluidV[grid_fluid_v_index(dispatchThreadId)] +
        gGridFluidW[grid_fluid_w_index(dispatchThreadId + uint3(0u, 0u, 1u))] -
        gGridFluidW[grid_fluid_w_index(dispatchThreadId)];
    gGridFluidDivergence[index] = value / gGridFluidStep.x;
}
