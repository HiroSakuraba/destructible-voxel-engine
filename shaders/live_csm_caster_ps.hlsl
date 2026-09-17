Texture2D<float4> gShadowBaseColor : register(t23);
Texture2D<float4> gShadowOpacity : register(t24);
SamplerState gShadowBaseSampler : register(s3);
SamplerState gShadowOpacitySampler : register(s4);

cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;
    uint4 gObjectIdentityAndFlags;
    float4 gObjectMaterialParameters;
};

void main(float4 position : SV_Position, float2 uv : TEXCOORD0, float alpha : TEXCOORD1) {
    const uint flags = gObjectIdentityAndFlags.w;
    const uint alphaMasked = flags & 1U;
    if (alphaMasked == 0U) return;
    const float baseColorAlpha = gShadowBaseColor.Sample(gShadowBaseSampler, uv).a;
    const float opacity = gShadowOpacity.Sample(gShadowOpacitySampler, uv).r;
    clip(alpha * baseColorAlpha * opacity - gObjectMaterialParameters.x);
}
