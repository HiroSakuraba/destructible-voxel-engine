#ifndef DVE_DASHR_LIVE_TRACE_HLSLI
#define DVE_DASHR_LIVE_TRACE_HLSLI

#include "dashr_surface.hlsli"

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
    float4x4 gObjectToWorld;
    float4 gCameraObjectAndHeightScale;
    float4 gCameraWorldAndDebug;
    float4 gEnvironmentParameters;
    float4 gHeightAndStep;
    float4 gDistortion;
    float4 gMinimumStepAndReserved;
    float4 gHeightUvScaleOffset;
    float4 gHeightUvRotation;
    uint4 gLimits;
};

struct DashrTraceEvaluation {
    float3 objectPosition;
    float3 surfacePosition;
    float3 normalObject;
    float sampledHeight;
    float delta;
    float stepFactor;
    bool valid;
};

struct DashrTraceHit {
    float3 objectPosition;
    float3 surfacePosition;
    float3 normalObject;
    uint stepsUsed;
    uint teleports;
    bool hit;
};

float2 DashrTransformHeightUv(float2 uv) {
    const float cosine = cos(gHeightUvRotation.x);
    const float sine = sin(gHeightUvRotation.x);
    const float2 scaled = uv * gHeightUvScaleOffset.xy;
    return float2(cosine * scaled.x - sine * scaled.y,
                  sine * scaled.x + cosine * scaled.y) +
           gHeightUvScaleOffset.zw;
}

DashrSurfaceSampleGpu DashrSampleSurface(float2 uv) {
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

bool DashrAtlasValid(float2 uv) {
    if (any(uv < float2(0.0F,0.0F)) || any(uv > float2(1.0F,1.0F))) return false;
    return gDashrAtlas2.SampleLevel(gDashrLinearClamp, uv, 0.0F).a > 0.25F &&
           gDashrAtlas3.SampleLevel(gDashrLinearClamp, uv, 0.0F).a > 0.25F;
}

float DashrSampleHeight(float2 uv) {
    const float2 mappedUv = DashrTransformHeightUv(uv);
    const float normalizedHeight =
        gDashrHeight.SampleLevel(gDashrHeightSampler, mappedUv, 0.0F).r;
    return DashrMapHeight(normalizedHeight, gCameraObjectAndHeightScale.w,
                          gHeightAndStep.x, gHeightAndStep.y);
}

DashrTraceEvaluation DashrEvaluateTracePoint(
    float3 startObject, float3 directionObject, float distance, float2 seedUv) {
    DashrTraceEvaluation result = (DashrTraceEvaluation)0;
    result.objectPosition = startObject + directionObject * distance;
    if (!DashrAtlasValid(seedUv)) return result;
    const DashrSurfaceSampleGpu surfaceSample = DashrSampleSurface(seedUv);
    result.surfacePosition = DashrObjectToSurface(
        surfaceSample, result.objectPosition,
        gDistortion.y, gDistortion.z, gDistortion.w,
        gMinimumStepAndReserved.x, result.stepFactor);
    if (any(result.surfacePosition.xy < float2(0.0F,0.0F)) ||
        any(result.surfacePosition.xy > float2(1.0F,1.0F))) return result;
    result.sampledHeight = DashrSampleHeight(result.surfacePosition.xy);
    result.delta = result.sampledHeight - result.surfacePosition.z;
    result.normalObject = normalize(surfaceSample.surfaceFromObjectRow2);
    result.valid = all(isfinite(result.surfacePosition)) &&
                   all(isfinite(result.normalObject)) &&
                   isfinite(result.sampledHeight) && isfinite(result.delta);
    return result;
}

DashrTraceHit DashrTraceSurface(float3 startObject, float2 initialUv) {
    DashrTraceHit output = (DashrTraceHit)0;
    const float3 rayVector = startObject - gCameraObjectAndHeightScale.xyz;
    const float rayLengthSquared = dot(rayVector, rayVector);
    if (rayLengthSquared <= 1.0e-12F) return output;
    const float3 directionObject = rayVector * rsqrt(rayLengthSquared);

    const float mapped0 = DashrMapHeight(
        0.0F, gCameraObjectAndHeightScale.w, gHeightAndStep.x, gHeightAndStep.y);
    const float mapped1 = DashrMapHeight(
        1.0F, gCameraObjectAndHeightScale.w, gHeightAndStep.x, gHeightAndStep.y);
    const float envelopeLow = min(mapped0, mapped1) - gHeightAndStep.z;
    const float envelopeHigh = max(mapped0, mapped1) + gHeightAndStep.z;

    float2 seedUv = initialUv;
    float distance = 0.0F;
    float nextStep = 0.0F;
    DashrTraceEvaluation previous = (DashrTraceEvaluation)0;
    float previousAcceptedDistance = 0.0F;
    bool havePreviousOutside = false;
    bool teleportCooldown = false;

    [loop]
    for (uint iteration = 0U; iteration < 192U; ++iteration) {
        if (iteration >= min(gLimits.x, 192U)) break;
        bool teleportedThisStep = false;

        const float seamRegion =
            gDashrSeamMap.SampleLevel(gDashrLinearClamp, seedUv, 0.0F).z;
        if (teleportCooldown) {
            if (seamRegion <= 0.0F) teleportCooldown = false;
        } else if (seamRegion > 0.0F) {
            if (output.teleports >= gLimits.z) return output;
            seedUv = gDashrSeamMap.SampleLevel(
                gDashrPointClamp, seedUv, 0.0F).xy;
            ++output.teleports;
            teleportedThisStep = true;
            teleportCooldown = true;
        }

        const float previousDistance = distance;
        distance += nextStep;
        DashrTraceEvaluation current =
            DashrEvaluateTracePoint(startObject, directionObject, distance, seedUv);
        if (!current.valid) return output;

        if (current.stepFactor < 1.0F && nextStep > 0.0F) {
            distance = previousDistance +
                nextStep * clamp(current.stepFactor, gMinimumStepAndReserved.x, 1.0F);
            current = DashrEvaluateTracePoint(
                startObject, directionObject, distance, seedUv);
            if (!current.valid) return output;
        }

        ++output.stepsUsed;
        if (current.surfacePosition.z < envelopeLow ||
            current.surfacePosition.z > envelopeHigh) return output;

        if (current.delta >= 0.0F) {
            DashrTraceEvaluation finalHit = current;
            if (havePreviousOutside && previous.delta < 0.0F &&
                !teleportedThisStep && gLimits.y > 0U) {
                DashrTraceEvaluation low = previous;
                DashrTraceEvaluation high = current;
                float lowDistance = previousAcceptedDistance;
                float highDistance = distance;
                [loop]
                for (uint refine = 0U; refine < 16U; ++refine) {
                    if (refine >= min(gLimits.y, 16U)) break;
                    const float middleDistance = (lowDistance + highDistance) * 0.5F;
                    const float2 middleSeed =
                        (low.surfacePosition.xy + high.surfacePosition.xy) * 0.5F;
                    DashrTraceEvaluation middle = DashrEvaluateTracePoint(
                        startObject, directionObject, middleDistance, middleSeed);
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
            output.objectPosition = finalHit.objectPosition;
            output.surfacePosition = finalHit.surfacePosition;
            output.normalObject = finalHit.normalObject;
            output.hit = true;
            return output;
        }

        previous = current;
        previousAcceptedDistance = distance;
        havePreviousOutside = true;
        seedUv = current.surfacePosition.xy;
        nextStep = gHeightAndStep.w *
                   max(1.0F, -current.delta * gDistortion.x);
    }
    return output;
}

float DashrDepthFromObjectPosition(float3 objectPosition) {
    const float4 clip = mul(gObjectToClip, float4(objectPosition, 1.0F));
    return clip.w > 1.0e-8F ? saturate(clip.z / clip.w) : 1.0F;
}

#endif
