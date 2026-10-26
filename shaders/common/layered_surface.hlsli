#ifndef DVE_LAYERED_SURFACE_HLSLI
#define DVE_LAYERED_SURFACE_HLSLI
#include "material_record.hlsli"
#include "live_material_sampling.hlsli"
#include "material_surface_detail.hlsli"
// gLayerData is supplied by the caller so the main and shadow passes share code.
#include "layer_record_load.hlsli"
float LayerFloat(uint o) { return asfloat(gLayerData[o]); }
int LayerAddress(int p,int size,uint wrap) {
    if(wrap==33071U)return clamp(p,0,size-1);
    int period=wrap==33648U?size*2:size;
    int x=((p%period)+period)%period;
    return x>=size?period-1-x:x;
}
float4 LayerTexel(uint image,uint table,int2 xy) {
    int2 size=int2(gLayerData[table+1],gLayerData[table+2]);
    xy=int2(LayerAddress(xy.x,size.x,gLayerData[image+1]),LayerAddress(xy.y,size.y,gLayerData[image+2]));
    uint v=gLayerData[gLayerData[table]+xy.y*size.x+xy.x];
    float4 c=float4(v&255U,(v>>8)&255U,(v>>16)&255U,v>>24)/255.0F;
    if(gLayerData[image+5]!=0)c.rgb=float3(
        c.r<=.04045F?c.r/12.92F:pow((c.r+.055F)/1.055F,2.4F),
        c.g<=.04045F?c.g/12.92F:pow((c.g+.055F)/1.055F,2.4F),
        c.b<=.04045F?c.b/12.92F:pow((c.b+.055F)/1.055F,2.4F));
    return c;
}
float4 LayerMip(uint image,uint level,float2 uv,bool linearFilter) {
    const uint table=image+9+level*3;
    // Hardware normalized texel centers, including seam-crossing address modes.
    float2 p=uv*float2(gLayerData[table+1],gLayerData[table+2])-.5F;
    if(!linearFilter)return LayerTexel(image,table,int2(floor(p+.5F)));
    int2 xy=int2(floor(p));float2 f=frac(p);
    return lerp(lerp(LayerTexel(image,table,xy),LayerTexel(image,table,xy+int2(1,0)),f.x),
                lerp(LayerTexel(image,table,xy+int2(0,1)),LayerTexel(image,table,xy+1),f.x),f.y);
}
float4 LayerImage(uint image,float2 uv,float4 fallback) {
    if(image==0)return fallback;
    // Bound enormous authored coordinates before converting to signed texel indices.
    if(!all(isfinite(uv)))return fallback;
    float2 size=float2(gLayerData[image+7],gLayerData[image+8]);
    float rho=max(length(ddx(uv)*size),length(ddy(uv)*size));
    uv.x=gLayerData[image+1]==33071U?saturate(uv.x):fmod(uv.x,2.0F);
    uv.y=gLayerData[image+2]==33071U?saturate(uv.y):fmod(uv.y,2.0F);
    float lod=clamp(log2(max(rho,1.0F)),0.0F,float(gLayerData[image]-1));
    uint filter=gLayerData[image+(rho>1?3:4)];
    uint level=uint(floor(lod+(gLayerData[image+3]==9728U?.5F:0.0F)));
    float4 a=LayerMip(image,level,uv,filter!=9728U);
    if(gLayerData[image+3]==9728U||level+1>=gLayerData[image])return a;
    return lerp(a,LayerMip(image,level+1,uv,filter!=9728U),frac(lod));
}
float2 LayerUv(uint image,PolygonMaterialMappingRecord m,float2 uv,float2 uv1) {
    return LiveMaterialUv(m,uv,uv1,image==0?0:gLayerData[image+6]);
}
float4 LayerMap(uint image,PolygonMaterialMappingRecord m,float2 uv,float2 uv1,
    float3 wp,float3 op,float3 N,float3x3 rotation,float4 fallback) {
    if(image==0)return fallback;
    if(m.mappingMode<2)return LayerImage(image,LayerUv(image,m,uv,uv1),fallback);
    bool local=m.mappingMode==3;float3 sn=local?mul(transpose(rotation),N):N;
    float3 weights=MaterialTriplanarWeights(sn,m.triplanarBlendSharpness);
    float2 x,y,z;LiveMaterialProjections(m,local?op:wp,x,y,z);
    return LayerImage(image,x,fallback)*weights.x+LayerImage(image,y,fallback)*weights.y+LayerImage(image,z,fallback)*weights.z;
}
float3 LayerNormal(uint image,PolygonMaterialMappingRecord m,float2 uv,float2 uv1,
    float3 wp,float3 op,float3 N,float3x3 rotation) {
    if(image==0||m.normalScale<=0)return N;
    if(m.mappingMode<2)return ApplyLiveDerivativeNormal(N,wp,LayerUv(image,m,uv,uv1),
        LayerMap(image,m,uv,uv1,wp,op,N,rotation,float4(.5F,.5F,1,1)).xyz,m.normalScale);
    bool local=m.mappingMode==3;float3 sn=normalize(local?mul(transpose(rotation),N):N);
    float3 weights=MaterialTriplanarWeights(sn,m.triplanarBlendSharpness);
    float2 x,y,z;LiveMaterialProjections(m,local?op:wp,x,y,z);
    float3 tx=LayerImage(image,x,float4(.5F,.5F,1,1)).xyz*2-1;
    float3 ty=LayerImage(image,y,float4(.5F,.5F,1,1)).xyz*2-1;
    float3 tz=LayerImage(image,z,float4(.5F,.5F,1,1)).xyz*2-1;
    tx.xy*=m.normalScale;ty.xy*=m.normalScale;tz.xy*=m.normalScale;
    float3 wx=float3(abs(tx.z)*sn.x,tx.y+sn.y,tx.x+sn.z);
    float3 wy=float3(ty.x+sn.x,abs(ty.z)*sn.y,ty.y+sn.z);
    float3 wz=float3(tz.x+sn.x,tz.y+sn.y,abs(tz.z)*sn.z);
    float3 result=normalize(wx*weights.x+wy*weights.y+wz*weights.z);
    return local?normalize(mul(rotation,result)):result;
}
float3 LayerRnm(float3 base,float3 detail,float coverage) {
    float3 t=normalize(base)+float3(0,0,1),u=normalize(detail)*float3(-1,-1,1);
    float3 r=normalize(t*dot(t,u)/max(t.z,1e-6F)-u);
    return normalize(lerp(base,r,coverage));
}
void ApplyLayeredSurface(float2 uv,float2 uv1,float3 wp,float3 op,float3 geometricN,
    float4 tangent,float3x3 rotation,inout float3 base,inout float metallic,
    inout float roughness,inout float3 emission,inout float alpha,inout float3 N,
    out float lastCoverage) {
    lastCoverage=0;if(gLayerData[0]==0)return;
    PolygonMaterialMappingRecord root=LoadLayerPolygonMaterialMappingRecord(gLayerData[1]);
    float height=LayerMap(gLayerData[2],root,uv,uv1,wp,op,geometricN,rotation,.5F.xxxx).r;
    float3 T=tangent.xyz-geometricN*dot(tangent.xyz,geometricN);
    if(dot(T,T)<1e-12F)T=abs(geometricN.z)<.9F?cross(float3(0,0,1),geometricN):cross(float3(0,1,0),geometricN);
    T=normalize(T);float3 B=normalize(cross(geometricN,T))*(tangent.w<0?-1:1);
    [loop] for(uint i=0;i<gLayerData[0];++i){
        uint at=gLayerData[4+i],c=at+89;
        MaterialRecord source=LoadLayerMaterialRecord(at);
        PolygonMaterialMappingRecord m=LoadLayerPolygonMaterialMappingRecord(at+29);
        uint maskImage=gLayerData[c+8];
        float mask=saturate(LayerMap(maskImage,root,uv,uv1,wp,op,geometricN,rotation,1.0F.xxxx).r*LayerFloat(c+3)+LayerFloat(c+4));
        float coverage=mask*saturate(LayerFloat(c));
        uint hi=gLayerData[c+9]!=0?gLayerData[c+9]:gLayerData[c+15];
        PolygonMaterialMappingRecord hm=m;if(gLayerData[c+9]!=0)hm=root;
        float h=LayerMap(hi,hm,uv,uv1,wp,op,geometricN,rotation,.5F.xxxx).r;
        if(LayerFloat(c+5)>0)coverage=saturate(coverage*2*saturate(.5F+((h-height)*LayerFloat(c+5)+LayerFloat(c+6))/max(1e-5F,LayerFloat(c+7))));
        lastCoverage=coverage;
        // Sampling stays outside varying control flow: gradients remain well-defined at masks.
        float4 bc=LayerMap(gLayerData[c+10],m,uv,uv1,wp,op,geometricN,rotation,1.0F.xxxx);
        float4 mr=LayerMap(gLayerData[c+11],m,uv,uv1,wp,op,geometricN,rotation,1.0F.xxxx);
        float3 rgb=MaterialBaseColorRGB(source)*bc.rgb;
        float r=saturate(source.roughness*mr.g);
        float3 sn=LayerNormal(gLayerData[c+12],m,uv,uv1,wp,op,geometricN,rotation);
        ApplySurfaceDetail(LoadLayerSurfaceDetailRecord(at+73),op,wp,uv,uv1,rotation,tangent,rgb,r,sn);
        float3 em=MaterialEmissiveRGB(source)*LayerMap(gLayerData[c+13],m,uv,uv1,wp,op,geometricN,rotation,1.0F.xxxx).rgb;
        float a=saturate(source.baseColorA*bc.a*LayerMap(gLayerData[c+14],m,uv,uv1,wp,op,geometricN,rotation,1.0F.xxxx).r);
        if(coverage<=0)continue;
        uint blend=gLayerData[c+1];
        base=blend==0?lerp(base,rgb,coverage):blend==1?lerp(base,base*rgb,coverage):max(0.0F.xxx,base+rgb*coverage);
        metallic=saturate(lerp(metallic,saturate(source.metallic*mr.b),coverage));roughness=saturate(lerp(roughness,r,coverage));
        emission=max(0.0F.xxx,emission+em*coverage);
        float3 bn=float3(dot(N,T),dot(N,B),dot(N,geometricN));
        float3 dn=float3(dot(sn,T),dot(sn,B),dot(sn,geometricN));
        float3 combined=LayerRnm(bn,dn,coverage);N=normalize(T*combined.x+B*combined.y+geometricN*combined.z);
        uint policy=gLayerData[c+2];
        if(policy==1)alpha=saturate(lerp(alpha,a,coverage));
        if(policy==2)alpha=saturate(lerp(alpha,alpha*a,coverage));
        if(policy==3)alpha=saturate(lerp(alpha,max(alpha,a),coverage));
        if(policy==4)alpha=a;
        height=lerp(height,h,coverage);
    }
}
#endif
