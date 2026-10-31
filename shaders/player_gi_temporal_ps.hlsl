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
    // y: 0/1 legacy valid/invalid, 2/3 DN-1 enabled valid/invalid.
    const bool dn1 = gGiTemporal.y >= 1.5F;
    const bool canReuse = (gGiTemporal.y < 0.5F || (dn1 && gGiTemporal.y < 2.5F));
    if (canReuse) {
        const float4 clip = mul(gPreviousViewProjection, float4(world, 1.0F));
        if (clip.w > 1.0e-4F) {
            const float2 previousUv = clip.xy / clip.w * 0.5F + 0.5F;
            if (all(previousUv >= 0.0F) && all(previousUv < 1.0F)) {
                const float4 previous = gGiHistory.Load(int3(int2(previousUv * gGiTemporal.zw), 0));
                const float expected = length(world - gPreviousCamera.xyz);
                if (previous.a > 0.0F && abs(previous.a - expected) < 0.06F * expected) {
                    history = previous.rgb;
                    if (dn1) {
                        // Motion-aware anti-ghosting: bound the reprojected
                        // light by valid, same-depth *current* radiance.
                        // This is a controlled DN-1 clamp, not SVGF moments.
                        const int2 size=int2(gGiTemporal.zw);
                        float3 lo=raw.rgb,hi=raw.rgb;
                        [unroll] for(int y=-1;y<=1;++y) {
                            [unroll] for(int x=-1;x<=1;++x) {
                                const int2 samplePixel=texel+int2(x,y);
                                if(any(samplePixel<0)||any(samplePixel>=size))continue;
                                const float4 neighbour=gGiRaw.Load(int3(samplePixel,0));
                                if(neighbour.a<.5F||!all(isfinite(neighbour.rgb))||
                                   any(neighbour.rgb<0.0F.xxx))continue;
                                bool surfaceValid;
                                const float3 p=PixelPosition(HalfTexelPixel(samplePixel),surfaceValid);
                                if(!surfaceValid)continue;
                                const float neighbourDistance=length(p-gCameraPosition.xyz);
                                if(abs(neighbourDistance-distanceToCamera)>
                                   max(.03F,.04F*distanceToCamera))continue;
                                lo=min(lo,neighbour.rgb);
                                hi=max(hi,neighbour.rgb);
                            }
                        }
                        const float3 slack=max(.02F.xxx,
                            (hi-lo)*.12F+max(raw.rgb,0.0F.xxx)*.05F);
                        history=clamp(max(history,0.0F.xxx),max(lo-slack,0.0F.xxx),hi+slack);
                    }
                    blend = gGiTemporal.x;
                }
            }
        }
    }
    return float4(lerp(history, raw.rgb, blend), distanceToCamera);
}
