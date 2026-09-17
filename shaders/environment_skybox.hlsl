#include "common/environment_ibl.hlsli"

TextureCube<float4> gSkyEnvironment : register(t18);
SamplerState gEnvironmentSampler : register(s1);
StructuredBuffer<float4> gSkyRayDirections : register(t21);
RWStructuredBuffer<float4> gSkyRadiance : register(u17);

cbuffer EnvironmentSkyboxConstants : register(b8) {
    uint gSkyboxElementCount;
    float gSkyboxRotationRadians;
    float gSkyboxIntensity;
    float gSkyboxExposure;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint i = dispatchThreadId.x;
    if (i >= gSkyboxElementCount) return;
    float3 direction = RotateEnvironmentDirectionY(normalize(gSkyRayDirections[i].xyz),
                                                   gSkyboxRotationRadians);
    float3 radiance = max(gSkyEnvironment.SampleLevel(gEnvironmentSampler, direction, 0.0F).rgb,
                          0.0F.xxx);
    radiance *= max(gSkyboxIntensity, 0.0F) * exp2(clamp(gSkyboxExposure, -32.0F, 32.0F));
    gSkyRadiance[i] = float4(radiance, 1.0F);
}
