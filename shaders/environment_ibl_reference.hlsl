#include "common/environment_ibl.hlsli"

TextureCube<float4> gDiffuseIrradiance : register(t18);
TextureCube<float4> gSpecularPrefilter : register(t19);
Texture2D<float2> gEnvironmentBrdfLut : register(t20);
SamplerState gEnvironmentSampler : register(s1);
StructuredBuffer<float4> gIblInput : register(t21);
RWStructuredBuffer<float4> gIblOutput : register(u17);

cbuffer IblReferenceConstants : register(b8) {
    uint gIblElementCount;
    float gMaximumSpecularLod;
    uint gIblPad0;
    uint gIblPad1;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint i = dispatchThreadId.x;
    if (i >= gIblElementCount) return;
    float4 record = gIblInput[i];
    float3 normal = normalize(float3(record.x, max(record.y, 1.0e-4F), record.z));
    float roughness = saturate(record.w);
    float3 value = EvaluateImageBasedLighting(
        gDiffuseIrradiance, gSpecularPrefilter, gEnvironmentBrdfLut, gEnvironmentSampler,
        normal, float3(0.0F, 0.0F, 1.0F), float3(0.8F, 0.7F, 0.6F),
        0.0F, roughness, 0.5F, 1.0F, gMaximumSpecularLod, 0.0F);
    gIblOutput[i] = float4(value, 1.0F);
}
