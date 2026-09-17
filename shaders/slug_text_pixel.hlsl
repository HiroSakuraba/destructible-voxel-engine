// DVE 3D Slug text pixel shader.
// SPDX-License-Identifier: MIT OR Apache-2.0
// Based on Slug reference code copyright 2017 by Eric Lengyel.

#include "common/slug.hlsli"

Texture2D<float4> gSlugCurveTexture : register(t20);
Texture2D<uint4> gSlugBandTexture : register(t21);

struct SlugTextPixelInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 renderCoordinate : TEXCOORD0;
    nointerpolation float4 bandTransform : TEXCOORD1;
    nointerpolation int4 glyphData : TEXCOORD2;
};

float4 main(SlugTextPixelInput input) : SV_Target0
{
    const float coverage = SlugCoverage(gSlugCurveTexture, gSlugBandTexture,
                                        input.renderCoordinate, input.bandTransform,
                                        input.glyphData);
    return input.color * coverage;
}
