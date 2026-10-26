// Transmittance table (256 x 64, node-centred): light kept from radius r at zenith cosine mu to
// the top of the atmosphere. Two quadratic-spaced pieces of 192 steps, bunched at the start and
// at the perigee of a descending ray (CPU: optical_depth_fine).

#include "common/atmosphere.hlsli"

RWStructuredBuffer<float4> gTransmittance : register(u0);

float3 OpticalDepthPiece(float r, float mu, float a, float b, bool denseAtA) {
    float3 depth = 0.0F.xxx;
    float previous = 0.0F;
    const uint steps = 192u;
    [loop]
    for (uint i = 0u; i < steps; ++i) {
        float q = float(i + 1u) / float(steps);
        float f = denseAtA ? q * q : 1.0F - (1.0F - q) * (1.0F - q);
        float dt = (f - previous) * (b - a);
        float tm = a + (previous + 0.5F * (f - previous)) * (b - a);
        previous = f;
        float ri = sqrt(r * r + tm * tm + 2.0F * r * mu * tm);
        depth += SampleMedium(ri - BottomRadius()).extinction * dt;
    }
    return depth;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= kTransmittanceWidth * kTransmittanceHeight) return;
    uint i = index % kTransmittanceWidth, j = index / kTransmittanceWidth;
    float r, mu;
    TransmittanceRMu(float2(float(i) / float(kTransmittanceWidth - 1u), float(j) / float(kTransmittanceHeight - 1u)), r, mu);
    float rayLength = DistanceToTop(r, mu);
    float perigee = -r * mu;
    float3 depth = (perigee > 0.0F && perigee < rayLength)
        ? OpticalDepthPiece(r, mu, 0.0F, perigee, false) + OpticalDepthPiece(r, mu, perigee, rayLength, true)
        : OpticalDepthPiece(r, mu, 0.0F, rayLength, true);
    gTransmittance[index] = float4(exp(-depth), 1.0F);
}
