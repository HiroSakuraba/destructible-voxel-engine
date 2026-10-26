// Multiple-scattering table (32 x 32, cell-centred; Hillaire 2020): for each altitude and sun
// elevation, second-order light L2 and transfer f_ms averaged over 16 x 16 directions with an
// isotropic phase, psi_ms = L2 / (1 - f_ms). 32 quadratic-spaced steps per direction.

#include "common/atmosphere.hlsli"

StructuredBuffer<float4> gTransmittanceTable : register(t0);
RWStructuredBuffer<float4> gMultipleScattering : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= kMultipleScatteringSize * kMultipleScatteringSize) return;
    uint i = index % kMultipleScatteringSize, j = index / kMultipleScatteringSize;
    float R = BottomRadius(), T = TopRadius();
    float muS = -1.0F + 2.0F * (float(i) + 0.5F) / float(kMultipleScatteringSize);
    float altitude = (T - R) * (float(j) + 0.5F) / float(kMultipleScatteringSize);
    float r = R + altitude;
    float3 sun = float3(sqrt(max(0.0F, 1.0F - muS * muS)), muS, 0.0F);
    const uint kDirections = 16u, kSteps = 32u;
    const float isoPhase = 1.0F / (4.0F * kAtmospherePi);
    float3 secondOrder = 0.0F.xxx, transfer = 0.0F.xxx;
    [loop]
    for (uint a = 0u; a < kDirections; ++a) {
        [loop]
        for (uint c = 0u; c < kDirections; ++c) {
            float cosTheta = 1.0F - 2.0F * (float(a) + 0.5F) / float(kDirections);
            float phi = 2.0F * kAtmospherePi * (float(c) + 0.5F) / float(kDirections);
            float sinTheta = sqrt(max(0.0F, 1.0F - cosTheta * cosTheta));
            float3 view = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
            float ground = DistanceToGround(r, altitude, view.y);
            float rayLength = ground >= 0.0F ? ground : DistanceToTop(r, view.y);
            float3 throughput = 1.0F.xxx, L = 0.0F.xxx, f = 0.0F.xxx;
            float previous = 0.0F;
            [loop]
            for (uint k = 0u; k < kSteps; ++k) {
                float q = float(k + 1u) / float(kSteps);
                float t1 = rayLength * q * q, dt = t1 - previous, tm = previous + 0.5F * dt;
                previous = t1;
                float3 pos = float3(0.0F, r, 0.0F) + view * tm;
                float rp = length(pos);
                AtmosphereMedium m = SampleMedium(rp - R);
                float3 scatter = m.rayleigh + m.mie.xxx;
                float3 segment = exp(-m.extinction * dt);
                float3 sunLight = SunTransmittance(gTransmittanceTable, rp, dot(pos, sun) / rp);
                L += throughput * StepIntegral(scatter * sunLight * isoPhase, m.extinction, segment, dt);
                f += throughput * StepIntegral(scatter, m.extinction, segment, dt);
                throughput *= segment;
            }
            if (ground >= 0.0F) {
                float3 up = normalize(float3(0.0F, r, 0.0F) + view * rayLength);
                float muG = dot(up, sun);
                L += throughput * SunTransmittance(gTransmittanceTable, R, muG) * (max(0.0F, muG) * gShape.w / kAtmospherePi);
            }
            secondOrder += L / float(kDirections * kDirections);
            transfer += f / float(kDirections * kDirections);
        }
    }
    float3 psi = secondOrder / max(1.0e-6F.xxx, 1.0F.xxx - transfer);
    gMultipleScattering[index] = float4(psi, 1.0F);
}
