// Full-resolution indirect light from the accumulated half-resolution history: a depth-aware
// blur over the 3x3 nearest half-resolution texels, so edges do not bleed across surfaces.
#include "common/player_gi_common.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    bool ok;
    const float3 world = PixelPosition(pixel, ok);
    if (gGridOrigin.w <= 0.0F || !ok) return float4(0.0F, 0.0F, 0.0F, 0.0F);
    const float distanceToCamera = length(world - gCameraPosition.xyz);
    const int2 halfSize = int2(gGiTemporal.zw);
    const int2 centre = clamp(int2(floor((float2(pixel) + 0.5F) * 0.5F)), int2(0, 0), halfSize - 1);
    [unroll] for (int pass = 0; pass < 2; ++pass) {
        const float tolerance = (pass == 0 ? 0.04F : 0.3F) * distanceToCamera;
        float3 sum = 0.0F.xxx;
        float total = 0.0F;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                const int2 texel = centre + int2(x, y);
                if (any(texel < 0) || any(texel >= halfSize)) continue;
                const float4 sample = gGiHistory.Load(int3(texel, 0));
                if (sample.a <= 0.0F) continue;
                const float depthError = (sample.a - distanceToCamera) / tolerance;
                const float w = exp(-0.5F * float(x * x + y * y)) * exp(-depthError * depthError);
                sum += sample.rgb * w;
                total += w;
            }
        }
        if (total > 1.0e-3F) return float4(sum / total, 1.0F);
    }
    return float4(0.0F, 0.0F, 0.0F, 0.0F);
}
