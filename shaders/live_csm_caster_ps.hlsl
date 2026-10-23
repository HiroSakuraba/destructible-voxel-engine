#include "common/live_bindings.hlsli"
#include "common/live_material_sampling.hlsli"
DVE_LIVE_BIND(2,3) StructuredBuffer<PolygonMaterialMappingRecord> gShadowMapping : register(t36);
DVE_LIVE_IMAGE(0,3) Texture2D<float4> gShadowBaseColor : register(t23);
DVE_LIVE_IMAGE(1,3) Texture2D<float4> gShadowOpacity : register(t24);
DVE_LIVE_IMAGE(0,3) SamplerState gShadowBaseSampler : register(s3);
DVE_LIVE_IMAGE(1,3) SamplerState gShadowOpacitySampler : register(s4);

DVE_LIVE_BIND(0,1) cbuffer LiveObjectConstants : register(b9) {
    float4x4 gObjectToWorld;
    uint4 gObjectIdentityAndFlags;
    float4 gObjectMaterialParameters;
};

void main(float4 position : SV_Position, float2 uv : TEXCOORD0, float alpha : TEXCOORD1,float2 uv1 : TEXCOORD2,float3 worldPosition : TEXCOORD3,
          float3 objectPosition : TEXCOORD4,float3 normal : TEXCOORD5) {
    const uint flags = gObjectIdentityAndFlags.w;
    const uint alphaMasked = flags & 1U;
    if (alphaMasked == 0U) return;
    const PolygonMaterialMappingRecord mapping=gShadowMapping[0];
    const float baseColorAlpha = SampleLiveMaterialMap(gShadowBaseColor,gShadowBaseSampler,mapping,uv,uv1,
        mapping.baseTexcoord,worldPosition,objectPosition,normal,(float3x3)gObjectToWorld).a;
    const float opacity = SampleLiveMaterialMap(gShadowOpacity,gShadowOpacitySampler,mapping,uv,uv1,
        (mapping.additionalTexcoords>>2U)&1U,worldPosition,objectPosition,normal,(float3x3)gObjectToWorld).r;
    clip(alpha * baseColorAlpha * opacity - gObjectMaterialParameters.x);
}
