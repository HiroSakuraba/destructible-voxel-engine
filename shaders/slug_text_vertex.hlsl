// DVE 3D Slug text vertex shader.
// SPDX-License-Identifier: MIT OR Apache-2.0
// Based on Slug reference code copyright 2017 by Eric Lengyel.

cbuffer SlugTextConstants : register(b4)
{
    float4 gSlugMvpRows[4];
    float4 gSlugViewport; // width, height, inverse width, inverse height
};

struct SlugTextVertexInput
{
    float3 position : POSITION;
    float2 dilationNormal : NORMAL0;
    float2 renderCoordinate : TEXCOORD0;
    float4 inverseJacobian : TEXCOORD1;
    float4 bandTransform : TEXCOORD2;
    uint4 glyphData : TEXCOORD3;
    float4 color : COLOR0;
};

struct SlugTextVertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 renderCoordinate : TEXCOORD0;
    nointerpolation float4 bandTransform : TEXCOORD1;
    nointerpolation int4 glyphData : TEXCOORD2;
};

SlugTextVertexOutput main(SlugTextVertexInput input)
{
    SlugTextVertexOutput output;
    float2 normal = normalize(input.dilationNormal);
    float s = dot(gSlugMvpRows[3].xyz, input.position) + gSlugMvpRows[3].w;
    float t = dot(gSlugMvpRows[3].xy, normal);
    float u = (s * dot(gSlugMvpRows[0].xy, normal) -
               t * (dot(gSlugMvpRows[0].xyz, input.position) + gSlugMvpRows[0].w)) * gSlugViewport.x;
    float v = (s * dot(gSlugMvpRows[1].xy, normal) -
               t * (dot(gSlugMvpRows[1].xyz, input.position) + gSlugMvpRows[1].w)) * gSlugViewport.y;
    float s2 = s * s;
    float st = s * t;
    float uv = u * u + v * v;
    float2 dilation = normal * (s2 * (st + sqrt(uv)) / max(uv - st * st, 1.0e-12));
    float3 position = input.position;
    position.xy += dilation;
    output.renderCoordinate = input.renderCoordinate +
        float2(dot(dilation, input.inverseJacobian.xy), dot(dilation, input.inverseJacobian.zw));
    output.position = float4(dot(gSlugMvpRows[0].xyz, position) + gSlugMvpRows[0].w,
                             dot(gSlugMvpRows[1].xyz, position) + gSlugMvpRows[1].w,
                             dot(gSlugMvpRows[2].xyz, position) + gSlugMvpRows[2].w,
                             dot(gSlugMvpRows[3].xyz, position) + gSlugMvpRows[3].w);
    output.bandTransform = input.bandTransform;
    output.glyphData = int4(input.glyphData);
    output.color = input.color;
    return output;
}
