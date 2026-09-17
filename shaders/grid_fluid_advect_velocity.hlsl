#include "common/grid_fluid.hlsli"

StructuredBuffer<float> gGridFluidU : register(t30);
StructuredBuffer<float> gGridFluidV : register(t31);
StructuredBuffer<float> gGridFluidW : register(t32);
StructuredBuffer<float> gGridFluidVelocityRead : register(t33);
RWStructuredBuffer<float> gGridFluidVelocityWrite : register(u20);

float sample_component(float3 worldPosition)
{
    uint3 dimensions = gGridFluidDimensions.xyz;
    float3 coordinate = worldPosition / gGridFluidStep.x - 0.5;
    if (gGridFluidDimensions.w == 0u) { dimensions.x += 1u; coordinate.x += 0.5; }
    if (gGridFluidDimensions.w == 1u) { dimensions.y += 1u; coordinate.y += 0.5; }
    if (gGridFluidDimensions.w == 2u) { dimensions.z += 1u; coordinate.z += 0.5; }
    coordinate = clamp(coordinate, 0.0, float3(dimensions - 1u));
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
    return lerp(lerp(lerp(gGridFluidVelocityRead[a000], gGridFluidVelocityRead[a100], t.x),
                     lerp(gGridFluidVelocityRead[a010], gGridFluidVelocityRead[a110], t.x), t.y),
                lerp(lerp(gGridFluidVelocityRead[a001], gGridFluidVelocityRead[a101], t.x),
                     lerp(gGridFluidVelocityRead[a011], gGridFluidVelocityRead[a111], t.x), t.y), t.z);
}

float sample_simple(StructuredBuffer<float> values, uint3 dimensions, float3 coordinate)
{
    coordinate = clamp(coordinate, 0.0, float3(dimensions - 1u));
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
    return lerp(lerp(lerp(values[a000], values[a100], t.x),
                     lerp(values[a010], values[a110], t.x), t.y),
                lerp(lerp(values[a001], values[a101], t.x),
                     lerp(values[a011], values[a111], t.x), t.y), t.z);
}

float3 sample_velocity(float3 worldPosition)
{
    const float inverseCell = 1.0 / gGridFluidStep.x;
    return float3(
        sample_simple(gGridFluidU, uint3(gGridFluidDimensions.x + 1u,
                                         gGridFluidDimensions.y, gGridFluidDimensions.z),
                      float3(worldPosition.x * inverseCell,
                             worldPosition.y * inverseCell - 0.5,
                             worldPosition.z * inverseCell - 0.5)),
        sample_simple(gGridFluidV, uint3(gGridFluidDimensions.x,
                                         gGridFluidDimensions.y + 1u, gGridFluidDimensions.z),
                      float3(worldPosition.x * inverseCell - 0.5,
                             worldPosition.y * inverseCell,
                             worldPosition.z * inverseCell - 0.5)),
        sample_simple(gGridFluidW, uint3(gGridFluidDimensions.x,
                                         gGridFluidDimensions.y, gGridFluidDimensions.z + 1u),
                      float3(worldPosition.x * inverseCell - 0.5,
                             worldPosition.y * inverseCell - 0.5,
                             worldPosition.z * inverseCell)));
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint3 dimensions = gGridFluidDimensions.xyz;
    float3 offset = 0.5;
    if (gGridFluidDimensions.w == 0u) { dimensions.x += 1u; offset.x = 0.0; }
    if (gGridFluidDimensions.w == 1u) { dimensions.y += 1u; offset.y = 0.0; }
    if (gGridFluidDimensions.w == 2u) { dimensions.z += 1u; offset.z = 0.0; }
    if (any(dispatchThreadId >= dimensions)) return;
    const uint index = dispatchThreadId.x + dimensions.x *
                       (dispatchThreadId.y + dimensions.y * dispatchThreadId.z);
    const float3 worldPosition = (float3(dispatchThreadId) + offset) * gGridFluidStep.x;
    const float3 firstVelocity = sample_velocity(worldPosition);
    const float3 midpoint = worldPosition - 0.5 * gGridFluidStep.y * firstVelocity;
    const float3 sourcePosition = worldPosition - gGridFluidStep.y * sample_velocity(midpoint);
    gGridFluidVelocityWrite[index] = sample_component(sourcePosition);
}
