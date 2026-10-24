#ifndef DVE_LIVE_CLEAR_GLASS_HLSLI
#define DVE_LIVE_CLEAR_GLASS_HLSLI
#include "dielectric_optics.hlsli"
struct OpticalNode {
    float3 minimum;uint escape;
    float3 maximum;uint firstTriangle;
    uint triangleCount;uint pad0;uint pad1;uint pad2;
};
struct OpticalTriangle {
    float3 p0;float3 p1;float3 p2;float3 n0;float3 n1;float3 n2;float3 c0;float3 c1;float3 c2;
    uint material;
};
struct OpticalSettings {
    uint nodeCount;uint triangleCount;uint materialCount;uint maximumInteractions;
    uint maximumPaths;uint mediumCount;float rayEpsilonMeters;float maximumDistanceMeters;
    uint4 media0;uint4 media1;
};
DVE_LIVE_BIND(0,4) StructuredBuffer<OpticalNode> gOpticalNodes : register(t37);
DVE_LIVE_BIND(1,4) StructuredBuffer<OpticalTriangle> gOpticalTriangles : register(t38);
DVE_LIVE_BIND(2,4) StructuredBuffer<MaterialRecord> gOpticalMaterials : register(t39);
DVE_LIVE_BIND(3,4) StructuredBuffer<OpticalSettings> gOpticalSettings : register(t40);
struct OpticalHit {float distance;uint surface;float2 bary;};
bool OpticalBox(float3 o,float3 d,OpticalNode node,float maximum) {
    float lo=0,hi=maximum;
    [unroll]for(uint axis=0;axis<3;++axis){
        if(abs(d[axis])<1e-20F){if(o[axis]<node.minimum[axis]||o[axis]>node.maximum[axis])return false;}
        else{const float a=(node.minimum[axis]-o[axis])/d[axis],b=(node.maximum[axis]-o[axis])/d[axis];lo=max(lo,min(a,b));hi=min(hi,max(a,b));}
    }return hi>=lo;
}
bool OpticalIntersect(float3 o,float3 d,OpticalTriangle t,float epsilon,inout OpticalHit hit) {
    const float3 a=t.p1-t.p0,b=t.p2-t.p0,q=cross(d,b);const float det=dot(a,q);
    if(abs(det)<1e-14F)return false;
    const float3 s=o-t.p0,r=cross(s,a);const float u=dot(s,q)/det,v=dot(d,r)/det,distance=dot(b,r)/det;
    if(u<0||v<0||u+v>1||distance<=epsilon||distance>=hit.distance)return false;
    hit.distance=distance;hit.bary=float2(u,v);return true;
}
bool TraceOpticalScene(float3 o,float3 d,OpticalSettings settings,out OpticalHit hit) {
    hit.distance=settings.maximumDistanceMeters;hit.surface=0xffffffffU;hit.bary=0;
    uint nodeIndex=0;
    [loop]while(nodeIndex<settings.nodeCount){const OpticalNode node=gOpticalNodes[nodeIndex];
        if(!OpticalBox(o,d,node,hit.distance)){nodeIndex=node.escape;continue;}
        if(node.triangleCount==0){++nodeIndex;continue;}
        [loop]for(uint i=node.firstTriangle;i<node.firstTriangle+node.triangleCount;++i)
            if(OpticalIntersect(o,d,gOpticalTriangles[i],settings.rayEpsilonMeters*.1F,hit))hit.surface=i;
        nodeIndex=node.escape;
    }return hit.surface!=0xffffffffU;
}
struct OpticalPath {
    float3 origin;float3 direction;float3 weight;uint depth;uint count;uint4 media0;uint4 media1;
};
uint OpticalMedium(OpticalPath path,uint index){return index<4?path.media0[index]:path.media1[index-4];}
void SetOpticalMedium(inout OpticalPath path,uint index,uint value){if(index<4)path.media0[index]=value;else path.media1[index-4]=value;}
float OpticalIor(uint medium){return medium==0?1:gOpticalMaterials[medium-1].indexOfRefraction;}
float3 OpticalOffset(float3 p,float3 geometricNormal,float3 outgoing,float epsilon){
    const float offset=max(epsilon,max(abs(p.x),max(abs(p.y),abs(p.z)))*2e-7F);
    return p+geometricNormal*(dot(outgoing,geometricNormal)>=0?offset:-offset);
}
float3 OpticalEnvironment(float3 direction){
    return gSpecularPrefilter.SampleLevel(gSpecularSampler,RotateEnvironmentDirectionY(direction,gEnvironmentParameters.x),0).rgb*max(0,gEnvironmentParameters.y);
}
float3 OpticalReceiver(MaterialRecord material,float3 N,float3 V,float3 color){
    const float3 light=EvaluateImageBasedLightingSeparateSamplers(gDiffuseIrradiance,gSpecularPrefilter,gEnvironmentBrdfLut,
        gEnvironmentSampler,gSpecularSampler,gBrdfSampler,N,V,MaterialBaseColorRGB(material)*color,
        material.metallic,material.roughness,material.specular,1,gEnvironmentParameters.w,gEnvironmentParameters.x)*max(0,gEnvironmentParameters.y);
    return light+MaterialEmissiveRGB(material);
}
float3 EvaluateLiveClearGlass(float3 origin,float3 direction) {
    const OpticalSettings settings=gOpticalSettings[0];
    OpticalPath paths[24];uint pending=1,processed=0;
    OpticalPath initial;initial.origin=origin;initial.direction=direction;initial.weight=1;initial.depth=0;
    initial.count=settings.mediumCount;initial.media0=settings.media0;initial.media1=settings.media1;paths[0]=initial;
    float3 radiance=0;
    [loop]while(pending>0&&processed<settings.maximumPaths){
        // Spend the finite budget on the largest remaining throughput first.
        uint best=0;float priority=-1;
        [loop]for(uint i=0;i<pending;++i){const float w=max(paths[i].weight.x,max(paths[i].weight.y,paths[i].weight.z));if(w>priority){priority=w;best=i;}}
        OpticalPath path=paths[best];paths[best]=paths[--pending];++processed;
        if(path.depth>=settings.maximumInteractions||max(path.weight.x,max(path.weight.y,path.weight.z))<1e-5F)continue;
        OpticalHit hit;const bool found=TraceOpticalScene(path.origin,path.direction,settings,hit);
        const uint medium=path.count?OpticalMedium(path,path.count-1):0;
        if(medium){const MaterialRecord inside=gOpticalMaterials[medium-1];path.weight*=ClearAbsorption(float3(inside.attenuationColorR,inside.attenuationColorG,inside.attenuationColorB),inside.attenuationDistanceMeters,hit.distance);}
        if(!found){radiance+=path.weight*OpticalEnvironment(path.direction);continue;}
        const OpticalTriangle surface=gOpticalTriangles[hit.surface];const MaterialRecord material=gOpticalMaterials[surface.material];
        const float3 p=path.origin+path.direction*hit.distance,Ng=normalize(cross(surface.p1-surface.p0,surface.p2-surface.p0));
        float3 Ns=surface.n0*(1-hit.bary.x-hit.bary.y)+surface.n1*hit.bary.x+surface.n2*hit.bary.y;
        Ns=dot(Ns,Ns)>1e-12F?normalize(Ns):Ng;
        if(dot(Ns,Ng)<0)Ns=-Ns;
        const bool entering=dot(path.direction,Ng)<0;
        const float3 opposingGeometric=entering?Ng:-Ng;Ns=entering?Ns:-Ns;
        if(dot(path.direction,Ns)>=0)Ns=opposingGeometric;
        if(material.transmission<=0){const float3 color=surface.c0*(1-hit.bary.x-hit.bary.y)+surface.c1*hit.bary.x+surface.c2*hit.bary.y;
            radiance+=path.weight*OpticalReceiver(material,Ns,-path.direction,color);continue;}
        const uint hitMedium=surface.material+1;
        // Valid media are closed and nested: never guess a replacement IOR at a mismatched exit.
        if(!entering&&(path.count==0||medium!=hitMedium))continue;
        if(entering&&path.count>=8)continue;
        const uint nextMedium=entering?hitMedium:(path.count>1?OpticalMedium(path,path.count-2):0);
        const float n1=OpticalIor(medium),n2=OpticalIor(nextMedium),eta=n1/n2;
        float3 transmitted,reflected=reflect(path.direction,Ns);
        bool refracts=ClearRefract(path.direction,Ns,eta,transmitted);
        if(dot(reflected,opposingGeometric)<=0 || (refracts&&dot(transmitted,opposingGeometric)>=0)){
            Ns=opposingGeometric;reflected=reflect(path.direction,Ns);refracts=ClearRefract(path.direction,Ns,eta,transmitted);}
        const float F=refracts?ClearDielectricFresnel(-dot(path.direction,Ns),n1,n2):1;
        OpticalPath reflection=path;reflection.depth++;reflection.direction=reflected;reflection.weight*=F;
        reflection.origin=OpticalOffset(p,Ng,reflected,settings.rayEpsilonMeters);
        // Both branches remain deterministic; throughput priority bounds cost.
        if(pending<24&&F>0)paths[pending++]=reflection;
        if(refracts){path.depth++;path.direction=transmitted;path.weight*=(1-F)*eta*eta;
            path.origin=OpticalOffset(p,Ng,transmitted,settings.rayEpsilonMeters);
            if(entering){SetOpticalMedium(path,path.count,hitMedium);path.count++;}else path.count--;
            if(pending<24)paths[pending++]=path;
            else{
                uint least=0;float smallest=max(paths[0].weight.x,max(paths[0].weight.y,paths[0].weight.z));
                [loop]for(uint i=1;i<24;++i){const float w=max(paths[i].weight.x,max(paths[i].weight.y,paths[i].weight.z));if(w<smallest){smallest=w;least=i;}}
                if(max(path.weight.x,max(path.weight.y,path.weight.z))>smallest)paths[least]=path;
            }
        }
    }
    return max(0,radiance)*exp2(clamp(gEnvironmentParameters.z,-32.F,32.F));
}
#endif
