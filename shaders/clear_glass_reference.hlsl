#include "common/dielectric_optics.hlsli"
struct DielectricProbe {float cosI;float n1;float n2;float distance;float3 color;float reference;};
[[vk::binding(0,0)]] StructuredBuffer<DielectricProbe> gDielectricProbes : register(t0);
[[vk::binding(1,0)]] RWStructuredBuffer<float4> gDielectricResults : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count,stride;gDielectricProbes.GetDimensions(count,stride);if(id.x>=count)return;
    const DielectricProbe p=gDielectricProbes[id.x];float3 direction=float3(sqrt(max(0,1-p.cosI*p.cosI)),0,-p.cosI),T;
    const bool ok=ClearRefract(direction,float3(0,0,1),p.n1/p.n2,T);
    gDielectricResults[id.x*2]=float4(ClearDielectricFresnel(p.cosI,p.n1,p.n2),T.x,T.z,ok?1:0);
    gDielectricResults[id.x*2+1]=float4(ClearAbsorption(p.color,p.reference,p.distance),1);
}
