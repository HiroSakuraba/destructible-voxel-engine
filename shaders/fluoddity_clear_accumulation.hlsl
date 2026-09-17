// Portable baseline for Fluoddity trail deposition. Signed fixed-point atomics avoid
// vendor-specific floating-point image atomic extensions.
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

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint voxelCount = gSimulation.y * gSimulation.y * gSimulation.y;
    if (dispatchThreadId.x < voxelCount)
        gAccumulation[dispatchThreadId.x] = int4(0, 0, 0, 0);
}
