#include "common/grid_fluid.hlsli"

StructuredBuffer<uint> gGridFluidCellTypes : register(t30);
RWStructuredBuffer<float> gGridFluidDensity : register(u20);
RWStructuredBuffer<float> gGridFluidTemperature : register(u21);
RWStructuredBuffer<float> gGridFluidFuelField : register(u22);
RWStructuredBuffer<float> gGridFluidFlame : register(u23);

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!grid_fluid_inside_cell(dispatchThreadId)) return;
    const uint index = grid_fluid_cell_index(dispatchThreadId);
    if (gGridFluidCellTypes[index] != 0u) return;
    float density = gGridFluidDensity[index];
    float temperature = gGridFluidTemperature[index];
    float fuel = gGridFluidFuelField[index];
    float flame = gGridFluidFlame[index];
    if (fuel > 0.0 && temperature >= gGridFluidFuel.x)
    {
        const float burn = min(fuel, gGridFluidFuel.y * gGridFluidStep.y * fuel);
        fuel -= burn;
        temperature += burn * gGridFluidFuel.z;
        density += burn * gGridFluidOutputs.x;
        flame += burn * gGridFluidOutputs.y;
    }
    density *= exp(-gGridFluidSmoke.z * gGridFluidStep.y);
    temperature = gGridFluidStep.z +
                  (temperature - gGridFluidStep.z) *
                  exp(-gGridFluidSmoke.w * gGridFluidStep.y);
    fuel *= exp(-gGridFluidFuel.w * gGridFluidStep.y);
    flame *= exp(-gGridFluidOutputs.z * gGridFluidStep.y);
    gGridFluidDensity[index] = max(0.0, density);
    gGridFluidTemperature[index] = temperature;
    gGridFluidFuelField[index] = max(0.0, fuel);
    gGridFluidFlame[index] = max(0.0, flame);
}
