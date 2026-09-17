#include "common/environment_ibl.hlsli"
TextureCube<float4> gSkyEnvironment : register(t18);
SamplerState gEnvironmentSampler : register(s1);
cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gInverseViewProjection;
    float4 gEnvironmentParameters;
};
float4 main(float4 position : SV_Position, float3 direction : TEXCOORD0) : SV_Target0 {
    float3 rotated = RotateEnvironmentDirectionY(normalize(direction), gEnvironmentParameters.x);
    float3 color = max(gSkyEnvironment.SampleLevel(gEnvironmentSampler, rotated, 0.0F).rgb, 0.0F.xxx);
    color *= max(gEnvironmentParameters.y, 0.0F) * exp2(clamp(gEnvironmentParameters.z, -32.0F, 32.0F));
    return float4(color, 1.0F);
}
