#ifndef DVE_LIVE_MATERIAL_SAMPLING_HLSLI
#define DVE_LIVE_MATERIAL_SAMPLING_HLSLI
#include "material_mapping.hlsli"

float2 LiveMaterialUv(PolygonMaterialMappingRecord m,float2 uv0,float2 uv1,uint channel) {
    return TransformMaterialUV((m.mappingMode==kMaterialMappingUV1 || channel==1)?uv1:uv0,
        float2(m.baseScaleX,m.baseScaleY),float2(m.baseOffsetX,m.baseOffsetY),m.baseRotationRadians);
}
void LiveMaterialProjections(PolygonMaterialMappingRecord m,float3 p,
                             out float2 x,out float2 y,out float2 z) {
    MaterialTriplanarCoordinates(p,m.triplanarScale,x,y,z);
    const float2 scale=float2(m.baseScaleX,m.baseScaleY),offset=float2(m.baseOffsetX,m.baseOffsetY);
    x=TransformMaterialUV(x,scale,offset,m.baseRotationRadians);
    y=TransformMaterialUV(y,scale,offset,m.baseRotationRadians);
    z=TransformMaterialUV(z,scale,offset,m.baseRotationRadians);
}
float4 SampleLiveMaterialMap(Texture2D<float4> image,SamplerState samplerState,
    PolygonMaterialMappingRecord m,float2 uv0,float2 uv1,uint channel,
    float3 worldPosition,float3 objectPosition,float3 worldNormal,float3x3 objectRotation) {
    if(m.mappingMode<kMaterialMappingWorldTriplanar){
        const float2 uv=LiveMaterialUv(m,uv0,uv1,channel);
        return image.SampleGrad(samplerState,uv,ddx(uv),ddy(uv));
    }
    const bool local=m.mappingMode==kMaterialMappingObjectTriplanar;
    const float3 p=local?objectPosition:worldPosition;
    const float3 N=local?mul(transpose(objectRotation),worldNormal):worldNormal;
    const float3 weights=MaterialTriplanarWeights(N,m.triplanarBlendSharpness);
    float2 x,y,z;LiveMaterialProjections(m,p,x,y,z);
    return image.SampleGrad(samplerState,x,ddx(x),ddy(x))*weights.x+
           image.SampleGrad(samplerState,y,ddx(y),ddy(y))*weights.y+
           image.SampleGrad(samplerState,z,ddx(z),ddy(z))*weights.z;
}
float3 ApplyLiveDerivativeNormal(float3 N,float3 worldPosition,float2 uv,float3 encoded,float strength) {
    N=normalize(N);
    if(strength<=0)return N;
    float3 n=encoded*2-1;n.xy*=strength;
    const float magnitude=dot(n,n);
    n=magnitude>1e-12F?n*rsqrt(magnitude):float3(0,0,1);
    const float3 dx=ddx(worldPosition),dy=ddy(worldPosition);
    const float2 ux=ddx(uv),uy=ddy(uv);
    const float det=ux.x*uy.y-ux.y*uy.x;
    if(abs(det)<=1e-8F)return N;
    float3 T=(dx*uy.y-dy*ux.y)/det;
    const float3 rawB=(dy*ux.x-dx*uy.x)/det;
    T-=N*dot(N,T);
    if(dot(T,T)<=1e-12F)return N;
    T=normalize(T);
    const float3 B=normalize(cross(N,T))*(dot(cross(N,T),rawB)<0?-1:1);
    return normalize(T*n.x+B*n.y+N*n.z);
}
float3 SampleLiveMaterialNormal(Texture2D<float4> image,SamplerState samplerState,
    PolygonMaterialMappingRecord m,float2 uv0,float2 uv1,float3 worldPosition,
    float3 objectPosition,float3 N,float3x3 objectRotation) {
    if(m.normalScale<=0)return normalize(N);
    if(m.mappingMode<kMaterialMappingWorldTriplanar){
        const float2 uv=LiveMaterialUv(m,uv0,uv1,m.normalTexcoord);
        return ApplyLiveDerivativeNormal(N,worldPosition,uv,
            image.SampleGrad(samplerState,uv,ddx(uv),ddy(uv)).xyz,m.normalScale);
    }
    const bool local=m.mappingMode==kMaterialMappingObjectTriplanar;
    const float3 sourceN=local?mul(transpose(objectRotation),N):N;
    const float3 weights=MaterialTriplanarWeights(sourceN,m.triplanarBlendSharpness);
    float2 x,y,z;LiveMaterialProjections(m,local?objectPosition:worldPosition,x,y,z);
    float3 tx=image.SampleGrad(samplerState,x,ddx(x),ddy(x)).xyz*2-1;
    float3 ty=image.SampleGrad(samplerState,y,ddx(y),ddy(y)).xyz*2-1;
    float3 tz=image.SampleGrad(samplerState,z,ddx(z),ddy(z)).xyz*2-1;
    tx.xy*=m.normalScale;ty.xy*=m.normalScale;tz.xy*=m.normalScale;
    // Whiteout triplanar blend (matches dve::triplanar_whiteout_normal in
    // include/dve/triplanar_normal.hpp): a neutral (0,0,1) sample reproduces sn exactly.
    const float3 sn=normalize(sourceN);
    const float3 wx=float3(abs(tx.z)*sn.x,tx.y+sn.y,tx.x+sn.z);
    const float3 wy=float3(ty.x+sn.x,abs(ty.z)*sn.y,ty.y+sn.z);
    const float3 wz=float3(tz.x+sn.x,tz.y+sn.y,abs(tz.z)*sn.z);
    const float3 normal=normalize(wx*weights.x+wy*weights.y+wz*weights.z);
    return local?normalize(mul(objectRotation,normal)):normal;
}
#endif
