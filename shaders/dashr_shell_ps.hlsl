#include "common/dashr_live_trace.hlsli"

struct DashrShellPixelInput {
    float4 position : SV_Position;
    float3 objectPosition : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct DashrShellPixelOutput {
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

DashrShellPixelOutput main(DashrShellPixelInput input) {
    const DashrTraceHit hit = DashrTraceSurface(input.objectPosition, input.uv);
    if (!hit.hit) discard;

    DashrShellPixelOutput output;
    output.depth = DashrDepthFromObjectPosition(hit.objectPosition);

    const uint debugMode = (uint)gCameraWorldAndDebug.w;
    if (debugMode == 1U) {
        output.color = float4(hit.surfacePosition.xy, 0.0F, 1.0F);
    } else if (debugMode == 2U) {
        const float stepRatio = float(hit.stepsUsed) / max(float(gLimits.x), 1.0F);
        output.color = float4(stepRatio, 1.0F - stepRatio, 0.0F, 1.0F);
    } else if (debugMode == 3U) {
        const float teleportRatio = float(hit.teleports) / max(float(gLimits.z), 1.0F);
        output.color = float4(teleportRatio, 0.0F, 1.0F, 1.0F);
    } else {
        const float3 lightDirection = normalize(float3(0.35F, 0.7F, 0.6F));
        const float diffuse =
            0.2F + 0.8F * saturate(dot(hit.normalObject, lightDirection));
        output.color = float4(diffuse.xxx, 1.0F);
    }
    return output;
}
