// DVE v1.93 production sprite pixel contract.

struct SpriteVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

Texture2D<float4> gSpriteTexture : register(t0);
SamplerState gSpriteSampler : register(s0);

float4 main(SpriteVertexOutput input) : SV_Target0 {
    return gSpriteTexture.Sample(gSpriteSampler, input.uv) * input.color;
}
