#ifndef DVE_GRID_FLUID_HLSLI
#define DVE_GRID_FLUID_HLSLI

cbuffer GridFluidConstants : register(b5)
{
    uint4 gGridFluidDimensions; // xyz cell dimensions, w component/flags
    float4 gGridFluidStep;      // cell size, dt, ambient temperature, correction strength
    float4 gGridFluidForces;    // gravity xyz, temperature buoyancy
    float4 gGridFluidSmoke;     // smoke weight, vorticity, density dissipation, temperature dissipation
    float4 gGridFluidFuel;      // ignition, burn rate, heat release, fuel dissipation
    float4 gGridFluidOutputs;   // smoke yield, flame yield, flame dissipation, pressure tolerance
};

uint grid_fluid_cell_index(uint3 coordinate)
{
    return coordinate.x + gGridFluidDimensions.x *
           (coordinate.y + gGridFluidDimensions.y * coordinate.z);
}

uint grid_fluid_u_index(uint3 coordinate)
{
    const uint width = gGridFluidDimensions.x + 1u;
    return coordinate.x + width *
           (coordinate.y + gGridFluidDimensions.y * coordinate.z);
}

uint grid_fluid_v_index(uint3 coordinate)
{
    return coordinate.x + gGridFluidDimensions.x *
           (coordinate.y + (gGridFluidDimensions.y + 1u) * coordinate.z);
}

uint grid_fluid_w_index(uint3 coordinate)
{
    return coordinate.x + gGridFluidDimensions.x *
           (coordinate.y + gGridFluidDimensions.y * coordinate.z);
}

bool grid_fluid_inside_cell(uint3 coordinate)
{
    return all(coordinate < gGridFluidDimensions.xyz);
}


#endif
