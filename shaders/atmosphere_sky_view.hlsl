// Sky-view table (192 x 128, cell-centred) at the observer's altitude for the sun's elevation:
// radiance per unit top-of-atmosphere sun irradiance by azimuth from the sun (u = azimuth / pi)
// and zenith angle (v < 0.5 above the horizon, square-root spaced toward it). 32 quadratic-spaced
// steps of single scattering plus the multiple-scattering source, and the lit ground.

#include "common/atmosphere.hlsli"

StructuredBuffer<float4> gTransmittanceTable : register(t0);
StructuredBuffer<float4> gMultipleScatteringTable : register(t1);
RWStructuredBuffer<float4> gSkyView : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= kSkyViewWidth * kSkyViewHeight) return;
    uint i = index % kSkyViewWidth, j = index / kSkyViewWidth;
    float R = BottomRadius();
    float altitude = clamp(gView.x, 0.001F, TopRadius() - R - 0.001F);
    float r = R + altitude;
    float muS = clamp(gView.y, -1.0F, 1.0F);
    float3 sun = float3(sqrt(max(0.0F, 1.0F - muS * muS)), muS, 0.0F);
    // Zenith angle of the horizon: pi/2 plus the dip, atan2(sqrt(h (2R + h)), R).
    float horizon = 0.5F * kAtmospherePi + atan2(sqrt(altitude * (2.0F * R + altitude)), R);
    float u = (float(i) + 0.5F) / float(kSkyViewWidth), v = (float(j) + 0.5F) / float(kSkyViewHeight);
    float zenith;
    if (v < 0.5F) {
        float s = 1.0F - 2.0F * v;
        zenith = horizon * (1.0F - s * s);
    } else {
        float s = 2.0F * v - 1.0F;
        zenith = horizon + (kAtmospherePi - horizon) * s * s;
    }
    float azimuth = u * kAtmospherePi;
    float zc = cos(zenith), zs = sqrt(max(0.0F, 1.0F - zc * zc));
    float3 view = float3(zs * cos(azimuth), zc, zs * sin(azimuth));

    float ground = DistanceToGround(r, altitude, view.y);
    float rayLength = ground >= 0.0F ? ground : DistanceToTop(r, view.y);
    float nu = dot(view, sun);
    float phaseR = RayleighPhase(nu), phaseM = CornetteShanksPhase(gShape.z, nu);
    float3 L = 0.0F.xxx, throughput = 1.0F.xxx;
    float previous = 0.0F;
    const uint kSteps = 32u;
    [loop]
    for (uint k = 0u; k < kSteps; ++k) {
        float q = float(k + 1u) / float(kSteps);
        float t1 = rayLength * q * q, dt = t1 - previous, tm = previous + 0.5F * dt;
        previous = t1;
        float3 pos = float3(0.0F, r, 0.0F) + view * tm;
        float rp = length(pos);
        float muSample = dot(pos, sun) / rp;
        AtmosphereMedium m = SampleMedium(rp - R);
        float3 sunLight = SunTransmittance(gTransmittanceTable, rp, muSample);
        float3 scatter = m.rayleigh + m.mie.xxx;
        float3 source = (m.rayleigh * phaseR + m.mie.xxx * phaseM) * sunLight +
                        MultipleScattering(gMultipleScatteringTable, rp, muSample) * scatter;
        float3 segment = exp(-m.extinction * dt);
        L += throughput * StepIntegral(source, m.extinction, segment, dt);
        throughput *= segment;
    }
    if (ground >= 0.0F) {
        float3 up = normalize(float3(0.0F, r, 0.0F) + view * rayLength);
        float muG = dot(up, sun);
        L += throughput * SunTransmittance(gTransmittanceTable, R, muG) * (max(0.0F, muG) * gShape.w / kAtmospherePi);
    }
    gSkyView[index] = float4(L, 1.0F);
}
