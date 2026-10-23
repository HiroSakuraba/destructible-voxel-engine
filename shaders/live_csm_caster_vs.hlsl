#include "common/live_bindings.hlsli"
DVE_LIVE_BIND(0,0) cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection;
    float4 gEnvironmentParameters;
};
DVE_LIVE_BIND(0,1) cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;
    uint4 gObjectIdentityAndFlags;
    float4 gObjectMaterialParameters;
};
DVE_LIVE_BIND(0,2) cbuffer LiveCascadeConstants : register(b10) {
    float4 gCascadeCenterRadius;
    float4 gCascadeRight;
    float4 gCascadeUp;
    float4 gCascadeForward;
    float4 gCascadeDepthAndIndex;
    float4 gCascadeAtlasScaleOffset;
};
struct ShadowVertexInput { DVE_LIVE_LOCATION(0) float3 position : POSITION; DVE_LIVE_LOCATION(1) float3 normal : NORMAL; DVE_LIVE_LOCATION(3) float2 uv : TEXCOORD0; DVE_LIVE_LOCATION(5) float2 uv1 : TEXCOORD1; DVE_LIVE_LOCATION(4) float4 color : COLOR0; };
struct ShadowVertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; float alpha : TEXCOORD1; float2 uv1 : TEXCOORD2; float3 worldPosition : TEXCOORD3; float3 objectPosition : TEXCOORD4; float3 normal : TEXCOORD5; };
ShadowVertexOutput main(ShadowVertexInput input) {
    ShadowVertexOutput output;
    float3 world = mul(gObjectToWorld, float4(input.position, 1.0F)).xyz;
    float3 relative = world - gCascadeCenterRadius.xyz;
    float radius = max(gCascadeCenterRadius.w, 1.0e-5F);
    float depthRange = max(gCascadeDepthAndIndex.y - gCascadeDepthAndIndex.x, 1.0e-5F);
    // The sampler stores minimum/maximum depth in absolute light space. X and Y remain
    // centre-relative, but Z must use the same absolute origin or shadows become dependent on
    // distance from the world origin. Vulkan uses a positive-height viewport here, so NDC +Y
    // maps downward; negate the light-up projection to match atlas V=0 at the top.
    float depth = (dot(world, gCascadeForward.xyz) - gCascadeDepthAndIndex.x) / depthRange;
    output.position = float4(dot(relative, gCascadeRight.xyz) / radius,
                             -dot(relative, gCascadeUp.xyz) / radius,
                             max(depth, 0.0F), // depth pancaking keeps near-extruding casters alive
                             1.0F);
    output.uv = input.uv;output.uv1=input.uv1;output.worldPosition=world;
    output.objectPosition=input.position;output.normal=normalize(mul((float3x3)gObjectToWorld,input.normal));
    output.alpha = input.color.a * gObjectMaterialParameters.y;
    return output;
}
