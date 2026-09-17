// DVE adaptation of the Slug reference pixel algorithm.
// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2017 by Eric Lengyel. Adaptation copyright 2026 DVE contributors.
// The original source and required attribution are retained in third_party/slug_reference.

#ifndef DVE_SLUG_HLSLI
#define DVE_SLUG_HLSLI

static const uint kSlugLogBandTextureWidth = 12u;

uint SlugCalcRootCode(float y1, float y2, float y3)
{
    uint i1 = asuint(y1) >> 31u;
    uint i2 = asuint(y2) >> 30u;
    uint i3 = asuint(y3) >> 29u;
    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);
    return ((0x2E74u >> shift) & 0x0101u);
}

float2 SlugSolveHorizontal(float4 p12, float2 p3)
{
    float2 a = p12.xy - p12.zw * 2.0 + p3;
    float2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;
    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;
    if (abs(a.y) < 1.0 / 65536.0) t1 = t2 = p12.y * rb;
    return float2((a.x * t1 - b.x * 2.0) * t1 + p12.x,
                  (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

float2 SlugSolveVertical(float4 p12, float2 p3)
{
    float2 a = p12.xy - p12.zw * 2.0 + p3;
    float2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;
    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;
    if (abs(a.x) < 1.0 / 65536.0) t1 = t2 = p12.x * rb;
    return float2((a.y * t1 - b.y * 2.0) * t1 + p12.y,
                  (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

int2 SlugBandLocation(int2 glyphLocation, uint offset)
{
    int2 location = int2(glyphLocation.x + int(offset), glyphLocation.y);
    location.y += location.x >> kSlugLogBandTextureWidth;
    location.x &= (1 << kSlugLogBandTextureWidth) - 1;
    return location;
}

float SlugCombineCoverage(float xCoverage, float yCoverage, float xWeight, float yWeight, int flags)
{
    float coverage = max(abs(xCoverage * xWeight + yCoverage * yWeight) /
                         max(xWeight + yWeight, 1.0 / 65536.0),
                         min(abs(xCoverage), abs(yCoverage)));
    if ((flags & 0x1000) == 0) coverage = saturate(coverage);
    else coverage = 1.0 - abs(1.0 - frac(coverage * 0.5) * 2.0);
    return coverage;
}

float SlugCoverage(Texture2D<float4> curveData,
                   Texture2D<uint4> bandData,
                   float2 renderCoordinate,
                   float4 bandTransform,
                   int4 glyphData)
{
    float2 emsPerPixel = fwidth(renderCoordinate);
    float2 pixelsPerEm = 1.0 / emsPerPixel;
    int2 bandMaximum = glyphData.zw;
    bandMaximum.y &= 0x00FF;
    int2 bandIndex = clamp(int2(renderCoordinate * bandTransform.xy + bandTransform.zw),
                           int2(0, 0), bandMaximum);
    int2 glyphLocation = glyphData.xy;

    float xCoverage = 0.0;
    float xWeight = 0.0;
    uint2 horizontalBand = bandData.Load(int3(glyphLocation.x + bandIndex.y, glyphLocation.y, 0)).xy;
    int2 horizontalLocation = SlugBandLocation(glyphLocation, horizontalBand.y);
    for (int curveIndex = 0; curveIndex < int(horizontalBand.x); ++curveIndex)
    {
        int2 curveLocation = int2(bandData.Load(int3(horizontalLocation.x + curveIndex, horizontalLocation.y, 0)).xy);
        float4 p12 = curveData.Load(int3(curveLocation, 0)) - float4(renderCoordinate, renderCoordinate);
        float2 p3 = curveData.Load(int3(curveLocation.x + 1, curveLocation.y, 0)).xy - renderCoordinate;
        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;
        uint code = SlugCalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0u)
        {
            float2 roots = SlugSolveHorizontal(p12, p3) * pixelsPerEm.x;
            if ((code & 1u) != 0u)
            {
                xCoverage += saturate(roots.x + 0.5);
                xWeight = max(xWeight, saturate(1.0 - abs(roots.x) * 2.0));
            }
            if (code > 1u)
            {
                xCoverage -= saturate(roots.y + 0.5);
                xWeight = max(xWeight, saturate(1.0 - abs(roots.y) * 2.0));
            }
        }
    }

    float yCoverage = 0.0;
    float yWeight = 0.0;
    uint2 verticalBand = bandData.Load(int3(glyphLocation.x + bandMaximum.y + 1 + bandIndex.x,
                                            glyphLocation.y, 0)).xy;
    int2 verticalLocation = SlugBandLocation(glyphLocation, verticalBand.y);
    for (int curveIndex = 0; curveIndex < int(verticalBand.x); ++curveIndex)
    {
        int2 curveLocation = int2(bandData.Load(int3(verticalLocation.x + curveIndex, verticalLocation.y, 0)).xy);
        float4 p12 = curveData.Load(int3(curveLocation, 0)) - float4(renderCoordinate, renderCoordinate);
        float2 p3 = curveData.Load(int3(curveLocation.x + 1, curveLocation.y, 0)).xy - renderCoordinate;
        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;
        uint code = SlugCalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0u)
        {
            float2 roots = SlugSolveVertical(p12, p3) * pixelsPerEm.y;
            if ((code & 1u) != 0u)
            {
                yCoverage -= saturate(roots.x + 0.5);
                yWeight = max(yWeight, saturate(1.0 - abs(roots.x) * 2.0));
            }
            if (code > 1u)
            {
                yCoverage += saturate(roots.y + 0.5);
                yWeight = max(yWeight, saturate(1.0 - abs(roots.y) * 2.0));
            }
        }
    }
    return SlugCombineCoverage(xCoverage, yCoverage, xWeight, yWeight, glyphData.w);
}

#endif
