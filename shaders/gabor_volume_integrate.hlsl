#include "common/gabor_volume.hlsli"
cbuffer GaborVolumeConstants : register(b4) {
    uint primitiveCount; uint raySteps; uint mode; uint flags;
    float densityMultiplier; float lodBias; float maximumRayDistance; float temporalWeight;
    uint viewportWidth; uint viewportHeight; uint tileCountX; uint maximumPrimitivesPerTile;
    float3 cameraPosition; float stepLength;
};
StructuredBuffer<GaborPrimitive> gGaborPrimitives : register(t20);
StructuredBuffer<uint> gGaborTileCounts : register(t21);
StructuredBuffer<uint> gGaborTileIndices : register(t22);
StructuredBuffer<float> gSceneDepth : register(t23);
RWStructuredBuffer<float4> gGaborColorTransmittance : register(u10);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= viewportWidth || id.y >= viewportHeight) return;
    uint pixel = id.y * viewportWidth + id.x;
    uint tile = (id.y / 8u) * tileCountX + (id.x / 8u);
    uint count = min(gGaborTileCounts[tile], maximumPrimitivesPerTile);
    float3 rayDirection = normalize(float3((float(id.x)+0.5)/viewportWidth-0.5, 0.5-(float(id.y)+0.5)/viewportHeight, 1.0));
    float maximumDistance = min(maximumRayDistance, gSceneDepth[pixel]);
    float3 color = 0.0.xxx; float transmittance = 1.0;
    for (uint step = 0; step < raySteps && transmittance > 0.005; ++step) {
        float distance = (step + 0.5) * min(stepLength, maximumDistance / max(1u, raySteps));
        if (distance > maximumDistance) break;
        float3 position = cameraPosition + rayDirection * distance;
        float density = 0.0; float3 albedo = 0.0.xxx;
        for (uint slot = 0; slot < count; ++slot) {
            GaborPrimitive p = gGaborPrimitives[gGaborTileIndices[tile * maximumPrimitivesPerTile + slot]];
            float contribution = GaborDensity(p, position, 1.0);
            density += contribution; albedo += contribution * p.albedo;
        }
        if (density > 0.0) {
            albedo /= density;
            float alpha = 1.0 - exp(-density * densityMultiplier * stepLength);
            color += transmittance * alpha * albedo; transmittance *= 1.0 - alpha;
        }
    }
    gGaborColorTransmittance[pixel] = float4(color, transmittance);
}
