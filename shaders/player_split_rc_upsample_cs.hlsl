// Reconstruct the receiver at every full-resolution pixel. The old upsampler
// classified an entire 8x8 tile by its centre sample, which leaked indirect
// light onto foreground walls and even sky pixels within that tile.
// Match candidate probes to this pixel's actual surface before interpolation.
#include "common/player_screen_geometry.hlsli"
struct Surface { float4 position; float4 normal; };
DVE_LIVE_BIND(2,0) StructuredBuffer<Surface> surfaces : register(t1);
DVE_LIVE_BIND(3,0) StructuredBuffer<float4> light : register(t2);
DVE_LIVE_BIND(4,0) StructuredBuffer<float4> settings : register(t3);
DVE_LIVE_BIND(5,0) RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint width,height;
    output.GetDimensions(width,height);
    if(id.x>=width||id.y>=height)return;
    // Reject the sky and disoccluded surfaces, not just empty coarse tiles.
    float3 pixelPosition,pixelNormal;
    if(!SurfaceAtPixel(int2(id.xy),pixelPosition,pixelNormal)){
        output[id.xy]=0;
        return;
    }
    const uint columns=(width+7u)/8u,rows=(height+7u)/8u;
    uint surfaceCount,stride,lightCount,settingsCount;
    surfaces.GetDimensions(surfaceCount,stride);
    light.GetDimensions(lightCount,stride);
    settings.GetDimensions(settingsCount,stride);
    if(settingsCount==0u){output[id.xy]=0;return;}
    const float planeTolerance=max(0.0F,settings[0].x);
    const float2 g=(float2(id.xy)+.5F)/8.0F-.5F;
    const int2 base=(int2)floor(g);
    const float2 f=g-float2(base);
    float3 sum=0;
    float weightSum=0;
    [unroll]for(int oy=0;oy<2;++oy){
        [unroll]for(int ox=0;ox<2;++ox){
            const int2 q=base+int2(ox,oy);
            if(any(q<0)||q.x>=int(columns)||q.y>=int(rows))continue;
            const uint index=uint(q.y)*columns+uint(q.x);
            if(index>=surfaceCount||index>=lightCount)continue;
            const Surface other=surfaces[index];
            const float4 value=light[index];
            if(value.w<.5F||other.position.w<.5F||other.normal.w<.5F)continue;
            if(!all(isfinite(other.position))||!all(isfinite(other.normal))||
               !all(isfinite(value))||any(value.xyz<0))continue;
            const float normalAgreement=dot(pixelNormal,other.normal.xyz);
            // Check BOTH tangent planes: this rejects sampling across a near
            // perpendicular wall even when the pixel lies on the probe's plane.
            const float receiverPlane=abs(dot(pixelNormal,other.position.xyz-pixelPosition));
            const float probePlane=abs(dot(other.normal.xyz,pixelPosition-other.position.xyz));
            if(normalAgreement<.85F||receiverPlane>planeTolerance||probePlane>planeTolerance)continue;
            const float weight=(ox!=0?f.x:1.0F-f.x)*(oy!=0?f.y:1.0F-f.y);
            sum+=value.xyz*weight;
            weightSum+=weight;
        }
    }
    output[id.xy]=weightSum>1e-6F?float4(sum/weightSum,1):float4(0,0,0,0);
}
