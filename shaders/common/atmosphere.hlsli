// Physical atmosphere tables on the GPU: the same model, parameterisations and quadrature as the
// CPU (src/render/atmosphere.cpp), in kilometres so radii fit float precision. The observer's
// altitude travels separately because r - R loses it in float near the ground.

#ifndef DVE_ATMOSPHERE_HLSLI
#define DVE_ATMOSPHERE_HLSLI

static const float kAtmospherePi = 3.14159265358979323846F;
static const uint kTransmittanceWidth = 256u;
static const uint kTransmittanceHeight = 64u;
static const uint kMultipleScatteringSize = 32u;
static const uint kSkyViewWidth = 192u;
static const uint kSkyViewHeight = 128u;

cbuffer AtmosphereConstants : register(b0) {
    float4 gRadii;        // bottom radius, top radius, Rayleigh scale height, Mie scale height (km)
    float4 gRayleigh;     // Rayleigh scattering per km (rgb), Mie scattering per km
    float4 gOzone;        // ozone absorption per km at the peak (rgb), Mie absorption per km
    float4 gShape;        // ozone peak altitude, ozone half width (km), Mie anisotropy g, ground albedo
    float4 gView;         // observer altitude (km), sun zenith cosine, unused, unused
}

float BottomRadius() { return gRadii.x; }
float TopRadius() { return gRadii.y; }

struct AtmosphereMedium {
    float3 rayleigh;
    float mie;
    float3 extinction;
};

AtmosphereMedium SampleMedium(float altitude) {
    altitude = max(altitude, 0.0F);
    float rd = exp(-altitude / gRadii.z);
    float md = exp(-altitude / gRadii.w);
    float od = max(0.0F, 1.0F - abs(altitude - gShape.x) / gShape.y);
    AtmosphereMedium m;
    m.rayleigh = gRayleigh.xyz * rd;
    m.mie = gRayleigh.w * md;
    m.extinction = m.rayleigh + (gRayleigh.w + gOzone.w) * md + gOzone.xyz * od;
    return m;
}

float RayleighPhase(float c) { return 3.0F / (16.0F * kAtmospherePi) * (1.0F + c * c); }
float CornetteShanksPhase(float g, float c) {
    float k = 3.0F / (8.0F * kAtmospherePi) * (1.0F - g * g) / (2.0F + g * g);
    return k * (1.0F + c * c) / pow(max(1.0e-12F, 1.0F + g * g - 2.0F * g * c), 1.5F);
}

// Ray-sphere distances written to keep precision: T^2 - r^2 and r^2 - R^2 as products.
float DistanceToTop(float r, float mu) {
    float T = TopRadius();
    float disc = (T - r) * (T + r) + r * r * (mu * mu);
    return max(0.0F, -r * mu + sqrt(max(0.0F, disc)));
}
// altitude = r - R, passed in when known more precisely than r - R in float.
float DistanceToGround(float r, float altitude, float mu) {
    float R = BottomRadius();
    float disc = r * r * (mu * mu) - altitude * (2.0F * R + altitude);
    if (mu >= 0.0F || disc < 0.0F) return -1.0F;
    return max(0.0F, -r * mu - sqrt(disc));
}

// Node-centred transmittance table (Bruneton).
float2 TransmittanceUv(float r, float mu) {
    float R = BottomRadius(), T = TopRadius();
    r = clamp(r, R, T);
    mu = clamp(mu, -1.0F, 1.0F);
    float H = sqrt((T - R) * (T + R));
    float rho = sqrt(max(0.0F, (r - R) * (r + R)));
    float d = DistanceToTop(r, mu);
    float dMin = T - r, dMax = rho + H;
    return float2(dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0F, rho / H);
}
void TransmittanceRMu(float2 uv, out float r, out float mu) {
    float R = BottomRadius(), T = TopRadius();
    float H = sqrt((T - R) * (T + R));
    float rho = H * uv.y;
    r = sqrt(rho * rho + R * R);
    float dMin = T - r, dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0F ? 1.0F : clamp((H * H - rho * rho - d * d) / (2.0F * r * d), -1.0F, 1.0F);
}

float3 Bilinear(StructuredBuffer<float4> table, uint w, uint h, float x, float y) {
    x = clamp(x, 0.0F, float(w - 1u));
    y = clamp(y, 0.0F, float(h - 1u));
    uint x0 = min(uint(x), w - 2u), y0 = min(uint(y), h - 2u);
    float tx = x - float(x0), ty = y - float(y0);
    float3 a = table[y0 * w + x0].xyz, b = table[y0 * w + x0 + 1u].xyz;
    float3 c = table[(y0 + 1u) * w + x0].xyz, e = table[(y0 + 1u) * w + x0 + 1u].xyz;
    return lerp(lerp(a, b, tx), lerp(c, e, tx), ty);
}

float3 TransmittanceToTop(StructuredBuffer<float4> table, float r, float mu) {
    float2 uv = TransmittanceUv(r, mu);
    return Bilinear(table, kTransmittanceWidth, kTransmittanceHeight, uv.x * float(kTransmittanceWidth - 1u),
                    uv.y * float(kTransmittanceHeight - 1u));
}
float3 SunTransmittance(StructuredBuffer<float4> table, float r, float muSun) {
    float R = BottomRadius();
    r = max(r, R);
    if (DistanceToGround(r, r - R, muSun) >= 0.0F) return 0.0F.xxx;
    return TransmittanceToTop(table, r, muSun);
}
float3 MultipleScattering(StructuredBuffer<float4> table, float r, float muSun) {
    float u = saturate(0.5F + 0.5F * muSun);
    float v = saturate((r - BottomRadius()) / (TopRadius() - BottomRadius()));
    return Bilinear(table, kMultipleScatteringSize, kMultipleScatteringSize, u * float(kMultipleScatteringSize) - 0.5F,
                    v * float(kMultipleScatteringSize) - 0.5F);
}

// S (1 - e^{-sigma dt}) / sigma per channel: a constant source across a step.
float3 StepIntegral(float3 source, float3 extinction, float3 segment, float dt) {
    return float3(extinction.x > 1.0e-11F ? source.x * (1.0F - segment.x) / extinction.x : source.x * dt,
                  extinction.y > 1.0e-11F ? source.y * (1.0F - segment.y) / extinction.y : source.y * dt,
                  extinction.z > 1.0e-11F ? source.z * (1.0F - segment.z) / extinction.z : source.z * dt);
}

#endif
