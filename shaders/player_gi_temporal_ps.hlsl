// Temporal accumulation of the half-resolution one-bounce GI: reproject the surface into last
// frame's history, and blend the new rays into it when the history still shows the same surface.
#include "common/player_gi_common.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 texel = int2(position.xy);
    const float4 raw = gGiRaw.Load(int3(texel, 0));
    if (raw.a < 0.5F) return float4(0.0F, 0.0F, 0.0F, 0.0F);
    bool ok;
    const float3 world = PixelPosition(HalfTexelPixel(texel), ok);
    if (!ok) return float4(0.0F, 0.0F, 0.0F, 0.0F);
    const float distanceToCamera = length(world - gCameraPosition.xyz);
    float3 history = raw.rgb;
    float blend = 1.0F;
    if (gGiTemporal.y < 0.5F) {
        const float4 clip = mul(gPreviousViewProjection, float4(world, 1.0F));
        if (clip.w > 1.0e-4F) {
            const float2 previousUv = clip.xy / clip.w * 0.5F + 0.5F;
            if (all(previousUv >= 0.0F) && all(previousUv < 1.0F)) {
                const float4 previous = gGiHistory.Load(int3(int2(previousUv * gGiTemporal.zw), 0));
                const float expected = length(world - gPreviousCamera.xyz);
                if (previous.a > 0.0F && abs(previous.a - expected) < 0.06F * expected) {
                    history = previous.rgb;
                    blend = gGiTemporal.x;
                }
            }
        }
    }
    return float4(lerp(history, raw.rgb, blend), distanceToCamera);
}
