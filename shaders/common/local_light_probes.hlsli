#ifndef DVE_LOCAL_LIGHT_PROBES_HLSLI
#define DVE_LOCAL_LIGHT_PROBES_HLSLI
// The binding and stride ABI is mirrored by render/local_light_probes_gpu.hpp.
// All data is linear HDR. SH9 has already undergone Lambert cosine convolution.
#include "live_bindings.hlsli"
DVE_LIVE_BIND(3,0) StructuredBuffer<float4> gLocalProbeData : register(t44);
DVE_LIVE_IMAGE(4,0) TextureCube<float4> gLocalReflection0 : register(t45);
DVE_LIVE_IMAGE(4,0) SamplerState gLocalReflectionSampler0 : register(s15);
DVE_LIVE_IMAGE(5,0) TextureCube<float4> gLocalReflection1 : register(t46);
DVE_LIVE_IMAGE(5,0) SamplerState gLocalReflectionSampler1 : register(s16);

static const uint kLocalProbeHeader=4u;
static const uint kLocalProbeStride=10u;
static const uint kLocalReflectionStride=3u;

float3 LocalSH9Irradiance(float3 worldPosition, float3 worldNormal,
                          out float coverage) {
    coverage=0.0F;
    const float4 header=gLocalProbeData[0];
    if(header.w<.5F)return 0.0F.xxx;
    const uint3 dims=uint3(header.xyz);
    if(any(dims==0u) || any(dims>1024u) ||
       dims.x*dims.y*dims.z>1024u)return 0.0F.xxx;
    const float3 origin=gLocalProbeData[1].xyz;
    const float3 spacing=gLocalProbeData[2].xyz;
    if(any(spacing<=0.0F.xxx))return 0.0F.xxx;
    const float3 uv=(worldPosition-origin)/spacing;
    if(any(uv<0.0F.xxx) || any(uv>float3(dims-1u)))
        return 0.0F.xxx;
    const uint3 p0=uint3(floor(uv));
    const uint3 p1=min(p0+1u,dims-1u);
    const float3 t=uv-float3(p0);
    const float3 n=normalize(worldNormal);
    const float3 xyz=n;
    const float basis[9]={
       .282094791774F,
       .488602511903F*xyz.y,
       .488602511903F*xyz.z,
       .488602511903F*xyz.x,
       1.092548430592F*xyz.x*xyz.y,
       1.092548430592F*xyz.y*xyz.z,
       .315391565253F*(3.0F*xyz.z*xyz.z-1.0F),
       1.092548430592F*xyz.x*xyz.z,
       .546274215296F*(xyz.x*xyz.x-xyz.y*xyz.y)
    };
    float3 result=0.0F.xxx;
    [unroll]for(uint z=0u;z<2u;++z) {
        [unroll]for(uint y=0u;y<2u;++y) {
            [unroll]for(uint x=0u;x<2u;++x) {
                const float geometric=(x?t.x:1.0F-t.x)*
                                      (y?t.y:1.0F-t.y)*
                                      (z?t.z:1.0F-t.z);
                if(geometric<=0.0F)continue;
                const uint3 cell=uint3(x?p1.x:p0.x,y?p1.y:p0.y,z?p1.z:p0.z);
                const uint index=cell.x+dims.x*(cell.y+dims.y*cell.z);
                const uint base=kLocalProbeHeader+index*kLocalProbeStride;
                const float confidence=saturate(gLocalProbeData[base+9u].x);
                if(confidence<=0.0F)continue;
                float3 irradiance=0.0F.xxx;
                [unroll]for(uint k=0u;k<9u;++k) {
                    irradiance+=gLocalProbeData[base+k].xyz*basis[k];
                }
                const float w=geometric*confidence;
                result+=max(irradiance,0.0F.xxx)*w;
                coverage+=w;
            }
        }
    }
    coverage=saturate(coverage);
    return result;
}

float3 LocalBoxCorrectedDirection(float3 worldPosition,float3 reflected,
                                  float3 centre,float3 extents) {
    // Zero reflection components must contribute +infinity, never a
    // 0/0 NaN that can redirect reflections at the axis-aligned seams.
    float3 distances=1.0e20F.xxx;
    if(abs(reflected.x)>1.0e-6F)
        distances.x=(centre.x+(reflected.x>0.0F?extents.x:-extents.x)-worldPosition.x)/reflected.x;
    if(abs(reflected.y)>1.0e-6F)
        distances.y=(centre.y+(reflected.y>0.0F?extents.y:-extents.y)-worldPosition.y)/reflected.y;
    if(abs(reflected.z)>1.0e-6F)
        distances.z=(centre.z+(reflected.z>0.0F?extents.z:-extents.z)-worldPosition.z)/reflected.z;
    const float distance=max(0.0F,min(distances.x,min(distances.y,distances.z)));
    return normalize(worldPosition+reflected*distance-centre);
}

// Returns locally captured *radiance* and the fraction of specular lighting
// it owns. The environment is mixed by the caller using (1 - coverage).
float3 LocalSpecularRadiance(float3 worldPosition,float3 reflectionDirection,
                            float roughness,out float coverage) {
    coverage=0.0F;
    const float4 header=gLocalProbeData[0];
    const uint3 dims=header.w>.5F?uint3(header.xyz):uint3(0u,0u,0u);
    const uint start=kLocalProbeHeader+dims.x*dims.y*dims.z*kLocalProbeStride;
    const uint count=min(2u,(uint)gLocalProbeData[3].x);
    float weights[2]={0.0F,0.0F};
    float maxPriority=-1.0e10F;
    [unroll]for(uint k=0u;k<2u;++k) {
        if(k>=count)continue;
        const uint base=start+k*kLocalReflectionStride;
        const float4 origin=gLocalProbeData[base];
        const float4 extents=gLocalProbeData[base+1u];
        const float4 settings=gLocalProbeData[base+2u];
        const float3 interior=extents.xyz-abs(worldPosition-origin.xyz);
        const float nearest=min(interior.x,min(interior.y,interior.z));
        if(nearest<=0.0F)continue;
        const float fade=extents.w<=0.0F?1.0F:saturate(nearest/extents.w);
        weights[k]=max(settings.x,0.0F)*fade;
        if(weights[k]>0.0F)maxPriority=max(maxPriority,origin.w);
    }
    float total=0.0F;
    [unroll]for(uint k=0u;k<2u;++k) {
        if(k>=count)continue;
        const float priority=gLocalProbeData[start+k*kLocalReflectionStride].w;
        if(priority<maxPriority)weights[k]=0.0F;
        total+=weights[k];
    }
    if(total<=0.0F)return 0.0F.xxx;
    const float scale=1.0F/max(total,1.0F);
    float3 result=0.0F.xxx;
    [unroll]for(uint k=0u;k<2u;++k) {
        const float w=weights[k]*scale;
        if(w<=0.0F)continue;
        const uint base=start+k*kLocalReflectionStride;
        const float4 position=gLocalProbeData[base];
        const float4 extents=gLocalProbeData[base+1u];
        const float4 params=gLocalProbeData[base+2u];
        const float3 corrected=LocalBoxCorrectedDirection(worldPosition,reflectionDirection,
                                                           position.xyz,extents.xyz);
        const float lod=saturate(roughness)*params.y;
        result+=w*(k==0u?
            gLocalReflection0.SampleLevel(gLocalReflectionSampler0,corrected,lod).rgb:
            gLocalReflection1.SampleLevel(gLocalReflectionSampler1,corrected,lod).rgb);
        coverage+=w;
    }
    coverage=saturate(coverage);
    return result;
}
#endif
