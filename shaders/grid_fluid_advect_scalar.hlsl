#include "common/grid_fluid.hlsli"

StructuredBuffer<float> gGridFluidU : register(t30);
StructuredBuffer<float> gGridFluidV : register(t31);
StructuredBuffer<float> gGridFluidW : register(t32);
StructuredBuffer<float> gGridFluidScalarRead : register(t33);
StructuredBuffer<uint> gGridFluidCellTypes : register(t34);
RWStructuredBuffer<float> gGridFluidScalarWrite : register(u20);

float sample_scalar(float3 worldPosition)
{
    const float3 coordinate = clamp(worldPosition / gGridFluidStep.x - 0.5,
                                    0.0, float3(gGridFluidDimensions.xyz - 1u));
    const uint3 i0 = uint3(floor(coordinate));
    const uint3 i1 = min(i0 + 1u, gGridFluidDimensions.xyz - 1u);
    const float3 t = coordinate - float3(i0);
    const float c000 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i0.x, i0.y, i0.z))];
    const float c100 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i1.x, i0.y, i0.z))];
    const float c010 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i0.x, i1.y, i0.z))];
    const float c110 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i1.x, i1.y, i0.z))];
    const float c001 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i0.x, i0.y, i1.z))];
    const float c101 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i1.x, i0.y, i1.z))];
    const float c011 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i0.x, i1.y, i1.z))];
    const float c111 = gGridFluidScalarRead[grid_fluid_cell_index(uint3(i1.x, i1.y, i1.z))];
    const float c00 = lerp(c000, c100, t.x);
    const float c10 = lerp(c010, c110, t.x);
    const float c01 = lerp(c001, c101, t.x);
    const float c11 = lerp(c011, c111, t.x);
    return lerp(lerp(c00, c10, t.y), lerp(c01, c11, t.y), t.z);
}

float sample_u(float3 worldPosition)
{
    const uint3 dimensions = uint3(gGridFluidDimensions.x + 1u,
                                   gGridFluidDimensions.y,
                                   gGridFluidDimensions.z);
    const float3 coordinate = clamp(float3(worldPosition.x / gGridFluidStep.x,
                                           worldPosition.y / gGridFluidStep.x - 0.5,
                                           worldPosition.z / gGridFluidStep.x - 0.5),
                                    0.0, float3(dimensions - 1u));
    const uint3 i0 = uint3(floor(coordinate));
    const uint3 i1 = min(i0 + 1u, dimensions - 1u);
    const float3 t = coordinate - float3(i0);
    const uint width = dimensions.x;
    const uint slice = width * dimensions.y;
    const uint a000 = i0.x + width * i0.y + slice * i0.z;
    const uint a100 = i1.x + width * i0.y + slice * i0.z;
    const uint a010 = i0.x + width * i1.y + slice * i0.z;
    const uint a110 = i1.x + width * i1.y + slice * i0.z;
    const uint a001 = i0.x + width * i0.y + slice * i1.z;
    const uint a101 = i1.x + width * i0.y + slice * i1.z;
    const uint a011 = i0.x + width * i1.y + slice * i1.z;
    const uint a111 = i1.x + width * i1.y + slice * i1.z;
    return lerp(lerp(lerp(gGridFluidU[a000], gGridFluidU[a100], t.x),
                     lerp(gGridFluidU[a010], gGridFluidU[a110], t.x), t.y),
                lerp(lerp(gGridFluidU[a001], gGridFluidU[a101], t.x),
                     lerp(gGridFluidU[a011], gGridFluidU[a111], t.x), t.y), t.z);
}

float sample_v(float3 worldPosition)
{
    const uint3 dimensions = uint3(gGridFluidDimensions.x,
                                   gGridFluidDimensions.y + 1u,
                                   gGridFluidDimensions.z);
    const float3 coordinate = clamp(float3(worldPosition.x / gGridFluidStep.x - 0.5,
                                           worldPosition.y / gGridFluidStep.x,
                                           worldPosition.z / gGridFluidStep.x - 0.5),
                                    0.0, float3(dimensions - 1u));
    const uint3 i0 = uint3(floor(coordinate));
    const uint3 i1 = min(i0 + 1u, dimensions - 1u);
    const float3 t = coordinate - float3(i0);
    const uint width = dimensions.x;
    const uint slice = width * dimensions.y;
    const uint a000 = i0.x + width * i0.y + slice * i0.z;
    const uint a100 = i1.x + width * i0.y + slice * i0.z;
    const uint a010 = i0.x + width * i1.y + slice * i0.z;
    const uint a110 = i1.x + width * i1.y + slice * i0.z;
    const uint a001 = i0.x + width * i0.y + slice * i1.z;
    const uint a101 = i1.x + width * i0.y + slice * i1.z;
    const uint a011 = i0.x + width * i1.y + slice * i1.z;
    const uint a111 = i1.x + width * i1.y + slice * i1.z;
    return lerp(lerp(lerp(gGridFluidV[a000], gGridFluidV[a100], t.x),
                     lerp(gGridFluidV[a010], gGridFluidV[a110], t.x), t.y),
                lerp(lerp(gGridFluidV[a001], gGridFluidV[a101], t.x),
                     lerp(gGridFluidV[a011], gGridFluidV[a111], t.x), t.y), t.z);
}

float sample_w(float3 worldPosition)
{
    const uint3 dimensions = uint3(gGridFluidDimensions.x,
                                   gGridFluidDimensions.y,
                                   gGridFluidDimensions.z + 1u);
    const float3 coordinate = clamp(float3(worldPosition.x / gGridFluidStep.x - 0.5,
                                           worldPosition.y / gGridFluidStep.x - 0.5,
                                           worldPosition.z / gGridFluidStep.x),
                                    0.0, float3(dimensions - 1u));
    const uint3 i0 = uint3(floor(coordinate));
    const uint3 i1 = min(i0 + 1u, dimensions - 1u);
    const float3 t = coordinate - float3(i0);
    const uint width = dimensions.x;
    const uint slice = width * dimensions.y;
    const uint a000 = i0.x + width * i0.y + slice * i0.z;
    const uint a100 = i1.x + width * i0.y + slice * i0.z;
    const uint a010 = i0.x + width * i1.y + slice * i0.z;
    const uint a110 = i1.x + width * i1.y + slice * i0.z;
    const uint a001 = i0.x + width * i0.y + slice * i1.z;
    const uint a101 = i1.x + width * i0.y + slice * i1.z;
    const uint a011 = i0.x + width * i1.y + slice * i1.z;
    const uint a111 = i1.x + width * i1.y + slice * i1.z;
    return lerp(lerp(lerp(gGridFluidW[a000], gGridFluidW[a100], t.x),
                     lerp(gGridFluidW[a010], gGridFluidW[a110], t.x), t.y),
                lerp(lerp(gGridFluidW[a001], gGridFluidW[a101], t.x),
                     lerp(gGridFluidW[a011], gGridFluidW[a111], t.x), t.y), t.z);
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!grid_fluid_inside_cell(dispatchThreadId)) return;
    const uint index = grid_fluid_cell_index(dispatchThreadId);
    if (gGridFluidCellTypes[index] != 0u)
    {
        gGridFluidScalarWrite[index] = 0.0;
        return;
    }
    const float3 worldPosition = (float3(dispatchThreadId) + 0.5) * gGridFluidStep.x;
    const float3 velocity = float3(sample_u(worldPosition), sample_v(worldPosition),
                                   sample_w(worldPosition));
    const float3 midpoint = worldPosition - 0.5 * gGridFluidStep.y * velocity;
    const float3 midpointVelocity = float3(sample_u(midpoint), sample_v(midpoint),
                                           sample_w(midpoint));
    gGridFluidScalarWrite[index] = sample_scalar(
        worldPosition - gGridFluidStep.y * midpointVelocity);
}
