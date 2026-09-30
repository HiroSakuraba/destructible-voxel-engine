#include "common/dashr_live_trace.hlsli"

struct DashrShellPixelInput {
    float4 position : SV_Position;
    float3 objectPosition : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

float main(DashrShellPixelInput input) : SV_Depth {
    const float3 lightRayDirectionObject = gMinimumStepAndShadowRay.yzw;
    const DashrTraceHit hit = DashrTraceSurfaceAlong(
        input.objectPosition, input.uv, lightRayDirectionObject);
    if (!hit.hit) discard;
    return DashrDepthFromObjectPosition(hit.objectPosition);
}
