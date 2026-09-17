// DVE v2.07 indexed sprite pixel contract.
// Atlas R stores an exact 0-255 palette index encoded as UNORM8. The palette buffer stores
// packed little-endian RGBA8 values and is resolved before vertex tint/blending.

struct SpriteVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

Texture2D<float4> gSpriteIndexTexture : register(t0);
SamplerState gSpriteSampler : register(s0);
StructuredBuffer<uint> gSpritePalette : register(t1);

float4 unpack_rgba8(uint packed) {
    return float4(
        float(packed & 0xFFu),
        float((packed >> 8u) & 0xFFu),
        float((packed >> 16u) & 0xFFu),
        float((packed >> 24u) & 0xFFu)) / 255.0F;
}

float4 main(SpriteVertexOutput input) : SV_Target0 {
    const float encoded = gSpriteIndexTexture.Sample(gSpriteSampler, input.uv).r;
    const uint paletteIndex = min(255u, uint(round(saturate(encoded) * 255.0F)));
    return unpack_rgba8(gSpritePalette[paletteIndex]) * input.color;
}
