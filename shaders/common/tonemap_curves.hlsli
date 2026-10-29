#ifndef DVE_TONEMAP_CURVES_HLSLI
#define DVE_TONEMAP_CURVES_HLSLI

// Display transforms shared by the compute tonemap (tonemap.hlsl, structured buffers) and the
// raster present pass (player_present_ps.hlsl). Moved here verbatim from tonemap.hlsl.

float3 TonemapACES(float3 color) {
    // Narkowicz's fitted approximation of the ACES filmic curve - compact, no LUT needed, the
    // standard choice when a full ACES pipeline (RRT + ODT) isn't warranted.
    const float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 TonemapReinhard(float3 color) {
    return color / (1.0F.xxx + color);
}

// AgX (Wrensch's minimal approximation of Sobotka's AgX) and Khronos PBR
// Neutral, ported from NVIDIA nvpro_core2's tonemap_functions.h.slang
// (Apache-2.0); PBR Neutral adapts the Khronos Group reference. Both mix
// channels, unlike the per-channel curves above.
float3 TonemapAgX(float3 color) {
    const float3x3 agxMat = float3x3(0.842479062253094F, 0.0423282422610123F, 0.0423756549057051F,
                                     0.0784335999999992F, 0.878468636469772F, 0.0784336F,
                                     0.0792237451477643F, 0.0791661274605434F, 0.879142973793104F);
    color = mul(color, agxMat);
    const float minEv = -12.47393F, maxEv = 4.026069F;
    color = clamp(log2(max(color, 1.0e-10F.xxx)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    float3 v = 15.5F * color - 40.14F;
    v = v * color + 31.96F; v = v * color - 6.868F; v = v * color + 0.4298F;
    v = v * color + 0.1191F; v = v * color - 0.0023F;
    const float3x3 agxMatInv = float3x3(1.19687900512017F, -0.0528968517574562F, -0.0529716355144438F,
                                        -0.0980208811401368F, 1.15190312990417F, -0.0980434501171241F,
                                        -0.0990297440797205F, -0.0989611768448433F, 1.15107367264116F);
    return mul(v, agxMatInv);
}

float3 TonemapPBRNeutral(float3 color) {
    const float startCompression = 0.8F - 0.04F;
    const float desaturation = 0.15F;
    float x = min(color.x, min(color.y, color.z));
    float peak = max(color.x, max(color.y, color.z));
    float offset = x < 0.08F ? x * (-6.25F * x + 1.0F) : 0.04F;
    color -= offset;
    if (peak >= startCompression) {
        const float d = 1.0F - startCompression;
        float newPeak = 1.0F - d * d / (peak + d - startCompression);
        color *= newPeak / peak;
        float g = 1.0F - 1.0F / (desaturation * (peak - newPeak) + 1.0F);
        color = lerp(color, newPeak.xxx, g);
    }
    return color;
}

float LinearToSrgbChannel(float value) {
    // The real sRGB OETF (a linear segment near black, then a power curve), not the common
    // pow(x, 1/2.2) shortcut: the two agree almost everywhere but visibly diverge near black,
    // exactly where banding/crushed-shadow artifacts would be most noticeable.
    if (value <= 0.0031308F) return value * 12.92F;
    return 1.055F * pow(max(value, 0.0F), 1.0F / 2.4F) - 0.055F;
}

float3 LinearToSrgb(float3 color) {
    return float3(LinearToSrgbChannel(color.x), LinearToSrgbChannel(color.y), LinearToSrgbChannel(color.z));
}

#endif // DVE_TONEMAP_CURVES_HLSLI
