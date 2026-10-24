#include "common/material_record.hlsli"
#include "common/environment_ibl.hlsli"
#include "common/live_bindings.hlsli"
DVE_LIVE_IMAGE(0,2) TextureCube<float4> gDiffuseIrradiance : register(t18);
DVE_LIVE_IMAGE(1,2) TextureCube<float4> gSpecularPrefilter : register(t19);
DVE_LIVE_IMAGE(2,2) Texture2D<float2> gEnvironmentBrdfLut : register(t20);
DVE_LIVE_IMAGE(0,2) SamplerState gEnvironmentSampler : register(s1);
DVE_LIVE_IMAGE(1,2) SamplerState gSpecularSampler : register(s5);
DVE_LIVE_IMAGE(2,2) SamplerState gBrdfSampler : register(s6);
DVE_LIVE_BIND(0,0) cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection;float4 gEnvironmentParameters;float4 gCameraWorldPosition;
};
#include "common/live_clear_glass.hlsli"
float4 main(float4 position : SV_Position,float3 worldPosition : TEXCOORD0) : SV_Target0 {
    return float4(EvaluateLiveClearGlass(gCameraWorldPosition.xyz,normalize(worldPosition-gCameraWorldPosition.xyz)),1);
}
