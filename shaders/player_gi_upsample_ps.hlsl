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
    // DN-1: optional edge-aware 5x5 wavelet-kernel resolve at half
    // resolution. Existing 3x3 upsample is preserved when disabled.
    // One spatial level only; no variance moments or material IDs yet.
    if(gGiTemporal.y>=1.5F) {
        float3 centrePosition,centreNormal;
        if(SurfaceAtPixel(pixel,centrePosition,centreNormal)){
            const float4 reference=gGiHistory.Load(int3(centre,0));
            const float refLum=dot(max(reference.rgb,0.0F.xxx),
                                   float3(.2126F,.7152F,.0722F));
            const float lumSigma=max(.08F,.30F*sqrt(refLum+.01F));
            const float planeSigma=max(.025F,.025F*distanceToCamera);
            float3 irradiance=0.0F.xxx;
            float sumWeight=0.0F;
            [loop]for(int y=-2;y<=2;++y) {
                [loop]for(int x=-2;x<=2;++x) {
                    const int2 texel=centre+int2(x,y);
                    if(any(texel<0)||any(texel>=halfSize))continue;
                    const float4 sample=gGiHistory.Load(int3(texel,0));
                    if(sample.a<=0.0F||!all(isfinite(sample.rgb))||
                       any(sample.rgb<0.0F.xxx))continue;
                    const float depthDifference=abs(sample.a-distanceToCamera);
                    if(depthDifference>max(.05F,.06F*distanceToCamera))continue;
                    bool sampleValid;
                    const float3 other=PixelPosition(HalfTexelPixel(texel),sampleValid);
                    if(!sampleValid)continue;
                    const float planeDifference=abs(dot(centreNormal,other-centrePosition));
                    if(planeDifference>planeSigma*3.0F)continue;
                    const float lum=dot(sample.rgb,float3(.2126F,.7152F,.0722F));
                    const float spatial=(abs(x)==2?1.0F:abs(x)==1?4.0F:6.0F)*
                                        (abs(y)==2?1.0F:abs(y)==1?4.0F:6.0F);
                    const float w=spatial*
                        exp(-planeDifference/max(planeSigma,1.0e-5F))*
                        exp(-depthDifference/max(.03F,.04F*distanceToCamera))*
                        exp(-abs(lum-refLum)/lumSigma);
                    irradiance+=w*sample.rgb;
                    sumWeight+=w;
                }
            }
            if(sumWeight>1.0e-5F)return float4(irradiance/sumWeight,1.0F);
        }
    }
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
