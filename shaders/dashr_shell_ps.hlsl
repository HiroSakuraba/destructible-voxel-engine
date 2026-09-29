#include "common/dashr_surface.hlsli"

Texture2D<float4> gDashrAtlas0 : register(t0, space1);
Texture2D<float4> gDashrAtlas1 : register(t1, space1);
Texture2D<float4> gDashrAtlas2 : register(t2, space1);
Texture2D<float4> gDashrAtlas3 : register(t3, space1);
Texture2D<float4> gDashrSeamMap : register(t4, space1);
Texture2D<float4> gDashrHeight : register(t5, space1);
SamplerState gDashrLinearClamp : register(s6, space1);
SamplerState gDashrPointClamp : register(s7, space1);
SamplerState gDashrHeightSampler : register(s8, space1);

cbuffer DashrShellConstants : register(b0, space0) {
    float4x4 gObjectToClip;
    float4 gCameraAndHeightScale;
    float4 gHeightAndStep;
    float4 gDistortion;
    float4 gMinimumStepAndDebug;
    uint4 gLimits;
};

struct DashrShellPixelInput {
    float4 position : SV_Position;
    float3 objectPosition : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct DashrShellPixelOutput {
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

struct TraceEvaluation {
    float3 objectPosition;
    float3 surfacePosition;
    float3 normalObject;
    float sampledHeight;
    float delta;
    float stepFactor;
    bool valid;
};

DashrSurfaceSampleGpu SampleDashrSurface(float2 uv) {
    const float4 a0 = gDashrAtlas0.SampleLevel(gDashrLinearClamp, uv, 0.0F);
    const float4 a1 = gDashrAtlas1.SampleLevel(gDashrLinearClamp, uv, 0.0F);
    const float4 a2 = gDashrAtlas2.SampleLevel(gDashrLinearClamp, uv, 0.0F);
    const float4 a3 = gDashrAtlas3.SampleLevel(gDashrLinearClamp, uv, 0.0F);
    DashrSurfaceSampleGpu sample;
    sample.surfaceFromObjectRow0 = a0.xyz;
    sample.distortionU = a0.w;
    sample.surfaceFromObjectRow1 = a1.xyz;
    sample.distortionV = a1.w;
    sample.surfaceFromObjectRow2 = a2.xyz;
    sample._dashrPad0 = a2.w;
    sample.objectAnchor = a3.xyz;
    sample._dashrPad1 = a3.w;
    sample.uv = uv;
    sample._dashrPad2 = 0.0F.xx;
    return sample;
}

bool AtlasValid(float2 uv) {
    if (any(uv < 0.0F.xx) || any(uv > 1.0F.xx)) return false;
    return gDashrAtlas2.SampleLevel(gDashrLinearClamp, uv, 0.0F).a > 0.25F &&
           gDashrAtlas3.SampleLevel(gDashrLinearClamp, uv, 0.0F).a > 0.25F;
}

float SampleDashrHeight(float2 uv) {
    const float normalizedHeight = gDashrHeight.SampleLevel(gDashrHeightSampler, uv, 0.0F).r;
    return DashrMapHeight(normalizedHeight, gCameraAndHeightScale.w,
                          gHeightAndStep.x, gHeightAndStep.y);
}

TraceEvaluation EvaluateTracePoint(float3 startObject, float3 directionObject,
                                   float distance, float2 seedUv) {
    TraceEvaluation result = (TraceEvaluation)0;
    result.objectPosition = startObject + directionObject * distance;
    if (!AtlasValid(seedUv)) return result;
    const DashrSurfaceSampleGpu surfaceSample = SampleDashrSurface(seedUv);
    result.surfacePosition = DashrObjectToSurface(
        surfaceSample, result.objectPosition,
        gDistortion.y, gDistortion.z, gDistortion.w,
        gMinimumStepAndDebug.x, result.stepFactor);
    if (any(result.surfacePosition.xy < 0.0F.xx) ||
        any(result.surfacePosition.xy > 1.0F.xx)) return result;
    result.sampledHeight = SampleDashrHeight(result.surfacePosition.xy);
    result.delta = result.sampledHeight - result.surfacePosition.z;
    result.normalObject = normalize(surfaceSample.surfaceFromObjectRow2);
    result.valid = all(isfinite(result.surfacePosition)) &&
                   isfinite(result.sampledHeight) && isfinite(result.delta);
    return result;
}

DashrShellPixelOutput main(DashrShellPixelInput input) {
    const float3 cameraObject = gCameraAndHeightScale.xyz;
    const float3 rayVector = input.objectPosition - cameraObject;
    const float rayLengthSquared = dot(rayVector, rayVector);
    if (rayLengthSquared <= 1.0e-12F) discard;
    const float3 directionObject = rayVector * rsqrt(rayLengthSquared);

    const float mapped0 = DashrMapHeight(0.0F, gCameraAndHeightScale.w,
                                         gHeightAndStep.x, gHeightAndStep.y);
    const float mapped1 = DashrMapHeight(1.0F, gCameraAndHeightScale.w,
                                         gHeightAndStep.x, gHeightAndStep.y);
    const float envelopeLow = min(mapped0, mapped1) - gHeightAndStep.z;
    const float envelopeHigh = max(mapped0, mapped1) + gHeightAndStep.z;

    float2 seedUv = input.uv;
    float distance = 0.0F;
    float nextStep = 0.0F;
    TraceEvaluation previous = (TraceEvaluation)0;
    bool havePreviousOutside = false;
    bool teleportCooldown = false;
    bool hit = false;
    TraceEvaluation finalHit = (TraceEvaluation)0;
    uint stepsUsed = 0U;
    uint teleports = 0U;

    [loop]
    for (uint iteration = 0U; iteration < 192U; ++iteration) {
        if (iteration >= min(gLimits.x, 192U)) break;
        bool teleportedThisStep = false;

        const float seamRegion =
            gDashrSeamMap.SampleLevel(gDashrLinearClamp, seedUv, 0.0F).z;
        if (teleportCooldown) {
            if (seamRegion <= 0.0F) teleportCooldown = false;
        } else if (seamRegion > 0.0F) {
            if (teleports >= gLimits.z) break;
            seedUv = gDashrSeamMap.SampleLevel(gDashrPointClamp, seedUv, 0.0F).xy;
            ++teleports;
            teleportedThisStep = true;
            teleportCooldown = true;
        }

        const float previousDistance = distance;
        distance += nextStep;
        TraceEvaluation current =
            EvaluateTracePoint(input.objectPosition, directionObject, distance, seedUv);
        if (!current.valid) discard;

        if (current.stepFactor < 1.0F && nextStep > 0.0F) {
            distance = previousDistance +
                       nextStep * clamp(current.stepFactor, gMinimumStepAndDebug.x, 1.0F);
            current = EvaluateTracePoint(
                input.objectPosition, directionObject, distance, seedUv);
            if (!current.valid) discard;
        }

        ++stepsUsed;
        if (current.surfacePosition.z < envelopeLow ||
            current.surfacePosition.z > envelopeHigh) discard;

        if (current.delta >= 0.0F) {
            finalHit = current;
            if (havePreviousOutside && previous.delta < 0.0F &&
                !teleportedThisStep && gLimits.y > 0U) {
                TraceEvaluation low = previous;
                TraceEvaluation high = current;
                float lowDistance = previousDistance;
                float highDistance = distance;
                [loop]
                for (uint refine = 0U; refine < 16U; ++refine) {
                    if (refine >= min(gLimits.y, 16U)) break;
                    const float middleDistance = (lowDistance + highDistance) * 0.5F;
                    const float2 middleSeed =
                        (low.surfacePosition.xy + high.surfacePosition.xy) * 0.5F;
                    TraceEvaluation middle = EvaluateTracePoint(
                        input.objectPosition, directionObject, middleDistance, middleSeed);
                    if (!middle.valid) break;
                    if (middle.delta >= 0.0F) {
                        high = middle;
                        highDistance = middleDistance;
                    } else {
                        low = middle;
                        lowDistance = middleDistance;
                    }
                }
                finalHit = high;
            }
            hit = true;
            break;
        }

        previous = current;
        havePreviousOutside = true;
        seedUv = current.surfacePosition.xy;
        nextStep = gHeightAndStep.w * max(1.0F, -current.delta * gDistortion.x);
    }

    if (!hit) discard;

    const float4 hitClip = mul(gObjectToClip, float4(finalHit.objectPosition, 1.0F));
    if (hitClip.w <= 1.0e-8F) discard;

    DashrShellPixelOutput output;
    output.depth = saturate(hitClip.z / hitClip.w);

    const uint debugMode = (uint)gMinimumStepAndDebug.y;
    if (debugMode == 1U) {
        output.color = float4(finalHit.surfacePosition.xy, 0.0F, 1.0F);
    } else if (debugMode == 2U) {
        const float stepRatio = float(stepsUsed) / max(float(gLimits.x), 1.0F);
        output.color = float4(stepRatio, 1.0F - stepRatio, 0.0F, 1.0F);
    } else if (debugMode == 3U) {
        output.color = float4(float(teleports) / max(float(gLimits.z), 1.0F), 0.0F, 1.0F, 1.0F);
    } else {
        const float3 lightDirection = normalize(float3(0.35F, 0.7F, 0.6F));
        const float diffuse = 0.2F + 0.8F * saturate(dot(finalHit.normalObject, lightDirection));
        output.color = float4(diffuse.xxx, 1.0F);
    }
    return output;
}
