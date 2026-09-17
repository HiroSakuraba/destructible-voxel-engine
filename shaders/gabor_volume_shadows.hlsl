#include "common/gabor_volume.hlsli"
cbuffer GaborShadowConstants : register(b4) {
    uint primitiveCount; uint sampleCount; float densityMultiplier; float maximumRayDistance;
    float3 lightDirection; float shadowStrength;
};
StructuredBuffer<GaborPrimitive> gGaborPrimitives : register(t20);
StructuredBuffer<float4> gShadowRayOrigins : register(t21);
RWStructuredBuffer<float> gGaborShadowVisibility : register(u10);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= sampleCount) return;
    float3 origin = gShadowRayOrigins[id.x].xyz;
    float opticalDepth = 0.0;
    [loop] for (uint step=0; step<32u; ++step) {
        float3 position = origin - normalize(lightDirection) * ((step+0.5) * maximumRayDistance / 32.0);
        [loop] for (uint p=0; p<primitiveCount; ++p) opticalDepth += GaborDensity(gGaborPrimitives[p], position, 1.0);
    }
    gGaborShadowVisibility[id.x] = lerp(1.0, exp(-opticalDepth * densityMultiplier), saturate(shadowStrength));
}
