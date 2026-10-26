#ifndef DVE_MATERIAL_SURFACE_DETAIL_HLSLI
#define DVE_MATERIAL_SURFACE_DETAIL_HLSLI
// Original GPU implementation of DVE's CPU metric recipe (no third-party texture/code).
struct SurfaceDetailRecord {
    uint pattern; uint space; uint seed; float periodMeters;
    float rotationX; float rotationY; float rotationZ; float rotationW;
    float grainStretch; float colorVariation; float roughnessVariation; float bumpHeightMeters;
    float uvTileMeters; uint reserved0; uint reserved1; uint reserved2;
};
float3 SurfaceRotate(float4 q,float3 v) { return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v); }
float4 SurfaceNoise(float3 p,uint seed) {
    int3 cell=int3(floor(p));float3 f=p-float3(cell);
    float3 u=f*f*f*(f*(f*6-15)+10),du=30*f*f*(f-1)*(f-1);
    float4 result=0;
    [unroll] for(uint corner=0;corner<8;++corner){
        uint3 b=uint3(corner&1,(corner>>1)&1,corner>>2);
        uint3 c=uint3(cell)+b;
        uint h=c.x*0x8da6b343U^c.y*0xd8163841U^c.z*0xcb1ab31fU^seed;
        h^=h>>16;h*=0x7feb352dU;h^=h>>15;h*=0x846ca68bU;h^=h>>16;
        float v=float(h>>8)*(2.0F/16777215.0F)-1;
        float3 w=float3(b.x?u.x:1-u.x,b.y?u.y:1-u.y,b.z?u.z:1-u.z);
        float3 dw=float3(b.x?du.x:-du.x,b.y?du.y:-du.y,b.z?du.z:-du.z);
        result+=v*float4(w.x*w.y*w.z,dw.x*w.y*w.z,w.x*dw.y*w.z,w.x*w.y*dw.z);
    }
    return result;
}
void SurfaceWave(inout float4 result,float argument,float3 derivative,float bound,float weight,float width){
    float w=6.2831853F*width*bound,a=weight*exp(-.5F*w*w);
    result+=a*float4(sin(argument),derivative*cos(argument));
}
float4 SampleSurfacePattern(SurfaceDetailRecord d,float3 p,float footprint){
    if(d.pattern==0||d.pattern>5||d.periodMeters<.00001F||d.grainStretch<1||!all(isfinite(p))||!isfinite(footprint))return 0;
    float4 rotation=float4(d.rotationX,d.rotationY,d.rotationZ,d.rotationW);
    float3 q=SurfaceRotate(float4(-rotation.xyz,rotation.w),p)/d.periodMeters;
    if(!all(isfinite(q))||any(abs(q)>1e8F))return 0;
    float width=max(0,footprint)/d.periodMeters;
    float4 result=0;const float tau=6.2831853F;
    if(d.pattern==1){
        float phase=float(d.seed%65521)*.61803398875F;
        float warp=.2F*sin(q.x/d.grainStretch+phase);
        float y=q.y+warp,z=q.z+.17F*sin(q.x/d.grainStretch*.7F);
        float radius=sqrt(y*y+z*z+.04F);
        float3 dr=float3((y*.2F*cos(q.x/d.grainStretch+phase)+z*.119F*cos(q.x/d.grainStretch*.7F))/(radius*d.grainStretch),y/radius,z/radius);
        SurfaceWave(result,tau*radius+phase,dr*tau,1.4F,.85F,width);
        float3 f=float3(1/d.grainStretch,3.1F,1.7F);
        SurfaceWave(result,tau*dot(q,f)+phase*1.7F,f*tau,3.6F,.15F,width);
    }else{
        float3 stretch=d.pattern==3?float3(1,1/d.grainStretch,1):1.0F.xxx;
        const float frequencies[3]={1,2.03F,4.17F},weights[3]={.55F,.3F,.15F};
        [unroll] for(uint i=0;i<3;++i){
            float frequency=frequencies[i],w=tau*width*frequency;
            float weight=weights[i]*exp(-.5F*w*w);
            float4 n=SurfaceNoise(q*stretch*frequency,d.seed+i*0x9e3779b9U);
            result+=weight*float4(n.x,n.yzw*stretch*frequency);
        }
    }
    return float4(result.x,SurfaceRotate(rotation,result.yzw/d.periodMeters));
}
// The caller supplies metres and a rigid object's rotation. UV footprints come from the
// selected UV channel, while object/world footprints never depend on unused UVs.
void ApplySurfaceDetail(SurfaceDetailRecord d,float3 objectMeters,float3 worldMeters,
                       float2 uv0,float2 uv1,float3x3 objectRotation,float4 worldTangent,
                       inout float3 color,inout float roughness,inout float3 normal){
    if(d.pattern==0)return;
    bool uv=d.space>=2;
    float3 p=d.space==1?worldMeters:objectMeters;
    if(uv)p=float3((d.space==3?uv1:uv0)*d.uvTileMeters,0);
    float footprint=max(length(ddx(p)),length(ddy(p)));
    float4 sample=SampleSurfacePattern(d,p,footprint);
    color=saturate(color*(1+d.colorVariation*sample.x));
    roughness=saturate(roughness+d.roughnessVariation*sample.x);
    float3 gradient=sample.yzw;
    if(uv){
        float3 T=worldTangent.xyz-normal*dot(normal,worldTangent.xyz);
        if(dot(T,T)<1e-12F)T=cross(abs(normal.y)<.9F?float3(0,1,0):float3(1,0,0),normal);
        T=normalize(T);float3 B=normalize(cross(normal,T))*worldTangent.w;
        gradient=T*gradient.x+B*gradient.y;
    }else if(d.space==0)gradient=mul(objectRotation,gradient);
    gradient-=normal*dot(gradient,normal);
    float3 slope=gradient*d.bumpHeightMeters;
    slope/=max(1,length(slope));normal=normalize(normal-slope);
}
#endif
