#include "common/material_mapping.hlsli"

cbuffer MaterialMappingReferenceConstants : register(b8) {
    float4 gReferenceUvScaleOffset;
    float4 gReferencePositionDistance;
    float4 gReferenceNormalRotation;
    float4 gReferenceTriplanarAndFade;
};

RWStructuredBuffer<float4> gMaterialMappingReferenceOutput : register(u12);

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId != 0u)) return;
    const float2 transformed = TransformMaterialUV(
        gReferenceUvScaleOffset.xy, gReferenceUvScaleOffset.zw,
        float2(gReferencePositionDistance.w, gReferenceNormalRotation.w),
        gReferenceTriplanarAndFade.w);
    float2 projectionX;
    float2 projectionY;
    float2 projectionZ;
    MaterialTriplanarCoordinates(gReferencePositionDistance.xyz,
                                 gReferenceTriplanarAndFade.x,
                                 projectionX, projectionY, projectionZ);
    const float3 weights = MaterialTriplanarWeights(gReferenceNormalRotation.xyz,
                                                    gReferenceTriplanarAndFade.y);
    const float detailFade = MaterialDetailFade(gReferencePositionDistance.w,
                                                gReferenceTriplanarAndFade.z,
                                                gReferenceNormalRotation.w);
    gMaterialMappingReferenceOutput[0] = float4(transformed, projectionX);
    gMaterialMappingReferenceOutput[1] = float4(projectionY, projectionZ);
    gMaterialMappingReferenceOutput[2] = float4(weights, detailFade);
}
