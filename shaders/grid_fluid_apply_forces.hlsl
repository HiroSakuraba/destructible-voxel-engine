#include "common/grid_fluid.hlsli"

StructuredBuffer<float> gGridFluidDensity : register(t30);
StructuredBuffer<float> gGridFluidTemperature : register(t31);
StructuredBuffer<uint> gGridFluidCellTypes : register(t32);
RWStructuredBuffer<float> gGridFluidVelocityComponent : register(u20);

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint3 dimensions = gGridFluidDimensions.xyz;
    if (gGridFluidDimensions.w == 0u) dimensions.x += 1u;
    if (gGridFluidDimensions.w == 1u) dimensions.y += 1u;
    if (gGridFluidDimensions.w == 2u) dimensions.z += 1u;
    if (any(dispatchThreadId >= dimensions)) return;
    const uint index = dispatchThreadId.x + dimensions.x *
                       (dispatchThreadId.y + dimensions.y * dispatchThreadId.z);
    float force = gGridFluidForces[gGridFluidDimensions.w];
    if (gGridFluidDimensions.w == 1u && dispatchThreadId.y > 0u &&
        dispatchThreadId.y < gGridFluidDimensions.y)
    {
        const uint3 below = uint3(dispatchThreadId.x, dispatchThreadId.y - 1u,
                                  dispatchThreadId.z);
        const uint3 above = uint3(dispatchThreadId.x, dispatchThreadId.y,
                                  dispatchThreadId.z);
        const uint belowIndex = grid_fluid_cell_index(below);
        const uint aboveIndex = grid_fluid_cell_index(above);
        if (gGridFluidCellTypes[belowIndex] == 0u && gGridFluidCellTypes[aboveIndex] == 0u)
        {
            const float temperature = 0.5 *
                (gGridFluidTemperature[belowIndex] + gGridFluidTemperature[aboveIndex]);
            const float density = 0.5 *
                (gGridFluidDensity[belowIndex] + gGridFluidDensity[aboveIndex]);
            force += gGridFluidForces.w * (temperature - gGridFluidStep.z) -
                     gGridFluidSmoke.x * density;
        }
    }
    gGridFluidVelocityComponent[index] += force * gGridFluidStep.y;
}
