#include "common/live_bindings.hlsli"
DVE_LIVE_BIND(0,0) cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection; float4 gEnvironmentParameters; float4 gCameraWorldPosition;
};
DVE_LIVE_BIND(0,1) cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld; uint4 gObjectIdentityAndFlags; float4 gObjectMaterialParameters;
};
struct MaterialVertexInput {
    DVE_LIVE_LOCATION(0) float3 position : POSITION;
    DVE_LIVE_LOCATION(1) float3 normal : NORMAL;
    DVE_LIVE_LOCATION(2) float4 tangent : TANGENT;
    DVE_LIVE_LOCATION(3) float2 uv : TEXCOORD0;
    DVE_LIVE_LOCATION(4) float4 color : COLOR0;
    DVE_LIVE_LOCATION(5) float2 uv1 : TEXCOORD1;
};
struct MaterialVertexOutput {
    float4 position : SV_Position; float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1; float2 uv : TEXCOORD2; float4 color : COLOR0;
    float3 objectPosition : TEXCOORD3; float2 uv1 : TEXCOORD4; float4 tangent : TEXCOORD5;
};
MaterialVertexOutput main(MaterialVertexInput input) {
    MaterialVertexOutput output;float4 world=mul(gObjectToWorld,float4(input.position,1));
    output.position=mul(gViewProjection,world);output.worldPosition=world.xyz;
    output.normal=normalize(mul((float3x3)gObjectToWorld,input.normal));
    output.tangent=float4(mul((float3x3)gObjectToWorld,input.tangent.xyz),input.tangent.w);
    output.uv=input.uv;output.uv1=input.uv1;output.color=input.color;output.objectPosition=input.position;
    return output;
}
