cbuffer LiveEnvironmentFrameConstants : register(b8) {
    float4x4 gInverseViewProjection;
    float4 gEnvironmentParameters;
};
struct SkyboxVertexOutput { float4 position : SV_Position; float3 direction : TEXCOORD0; };
SkyboxVertexOutput main(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    float2 clip = uv * 2.0F - 1.0F;
    float4 world = mul(gInverseViewProjection, float4(clip.x, -clip.y, 1.0F, 1.0F));
    SkyboxVertexOutput output;
    output.position = float4(clip, 1.0F, 1.0F);
    output.direction = normalize(world.xyz / max(abs(world.w), 1.0e-5F));
    return output;
}
