cbuffer FluoddityConstants : register(b0)
{
    uint4 gSimulation;
    float4 gIntegration;
    float4 gBoundsAndDrag;
    float4 gField;
    uint4 gFlags;
    float4 gParameterData[24];
    float4 gRule[30];
};

RWStructuredBuffer<int4> gAccumulation : register(u2);
RWTexture3D<float4> gTrailRead : register(u3);
RWTexture3D<float4> gTrailWrite : register(u4);

uint flat_index(uint3 coordinate)
{
    return coordinate.x + gSimulation.y *
           (coordinate.y + gSimulation.y * coordinate.z);
}

uint3 clamp_coordinate(int3 coordinate)
{
    return uint3(clamp(coordinate, 0, int(gSimulation.y) - 1));
}

[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId >= gSimulation.y))
        return;
    const int3 coordinate = int3(dispatchThreadId);
    float4 neighborSum = 0.0;
    neighborSum += gTrailRead[clamp_coordinate(coordinate + int3( 1, 0, 0))];
    neighborSum += gTrailRead[clamp_coordinate(coordinate + int3(-1, 0, 0))];
    neighborSum += gTrailRead[clamp_coordinate(coordinate + int3( 0, 1, 0))];
    neighborSum += gTrailRead[clamp_coordinate(coordinate + int3( 0,-1, 0))];
    neighborSum += gTrailRead[clamp_coordinate(coordinate + int3( 0, 0, 1))];
    neighborSum += gTrailRead[clamp_coordinate(coordinate + int3( 0, 0,-1))];
    const float4 current = gTrailRead[dispatchThreadId];
    const float4 diffused = lerp(current, neighborSum / 6.0,
                                 saturate(gField.w));
    const int4 fixedContribution = gAccumulation[flat_index(dispatchThreadId)];
    const float4 contribution = float4(fixedContribution) /
                                max(gIntegration.y, 1.0);
    gTrailWrite[dispatchThreadId] =
        (diffused + contribution) * saturate(gField.z);
}
