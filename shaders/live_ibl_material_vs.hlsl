cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gViewProjection;
    float4 gEnvironmentParameters;
};
cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;
    uint4 gObjectIdentityAndFlags;
    float4 gObjectMaterialParameters;
};
struct MaterialVertexInput { float3 position : POSITION; float3 normal : NORMAL; float4 tangent : TANGENT; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct MaterialVertexOutput { float4 position : SV_Position; float3 worldPosition : TEXCOORD0; float3 normal : TEXCOORD1; float2 uv : TEXCOORD2; float4 color : COLOR0; };
MaterialVertexOutput main(MaterialVertexInput input) {
    MaterialVertexOutput output; float4 world = mul(gObjectToWorld, float4(input.position, 1.0F));
    output.position = mul(gViewProjection, world); output.worldPosition = world.xyz;
    output.normal = normalize(mul((float3x3)gObjectToWorld, input.normal)); output.uv = input.uv; output.color = input.color; return output;
}
