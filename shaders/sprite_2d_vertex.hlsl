// DVE v1.93 production sprite vertex contract.

struct SpriteVertexInput {
    float3 positionNdc : POSITION0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct SpriteVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

SpriteVertexOutput main(SpriteVertexInput input) {
    SpriteVertexOutput output;
    output.position = float4(input.positionNdc, 1.0F);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
