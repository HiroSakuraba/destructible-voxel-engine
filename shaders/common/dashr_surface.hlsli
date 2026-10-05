#ifndef DVE_DASHR_SURFACE_HLSLI
#define DVE_DASHR_SURFACE_HLSLI

// DVE clean implementation of the surface-space mapping used by Tom Forsyth's
// DASHR (Dynamically Animated Skinned Heightfield Rendering), released by the
// author under MIT-0 / public-domain terms. See third_party/DASHR-NOTICE.md.
//
// This file deliberately contains only renderer-neutral math. The production
// atlas pass will supply these samples from UV-space deformation textures.

struct DashrSurfaceSampleGpu {
    float3 surfaceFromObjectRow0;
    float distortionU;
    float3 surfaceFromObjectRow1;
    float distortionV;
    float3 surfaceFromObjectRow2;
    float _dashrPad0;
    float3 objectAnchor;
    float _dashrPad1;
    float2 uv;
    float2 _dashrPad2;
};

float DashrAxisScale(float ratio,
                     float compressionThreshold,
                     float stretchThreshold,
                     float stretchDamping,
                     float minimumStepFactor,
                     inout float stepFactor) {
    if (ratio <= compressionThreshold) {
        stepFactor = min(stepFactor, minimumStepFactor);
        return 1.0F;
    }
    if (ratio > stretchThreshold) {
        const float scale = rcp(max(stretchDamping * ratio, 1.0e-7F));
        stepFactor = min(stepFactor, clamp(scale, minimumStepFactor, 1.0F));
        return scale;
    }
    return 1.0F;
}

float3 DashrObjectToSurface(DashrSurfaceSampleGpu sample,
                            float3 objectPosition,
                            float compressionThreshold,
                            float stretchThreshold,
                            float stretchDamping,
                            float minimumStepFactor,
                            out float stepFactor) {
    stepFactor = 1.0F;
    const float scaleU = DashrAxisScale(
        sample.distortionU, compressionThreshold, stretchThreshold,
        stretchDamping, minimumStepFactor, stepFactor);
    const float scaleV = DashrAxisScale(
        sample.distortionV, compressionThreshold, stretchThreshold,
        stretchDamping, minimumStepFactor, stepFactor);

    const float3 objectOffset = objectPosition - sample.objectAnchor;
    const float3 local = float3(
        dot(sample.surfaceFromObjectRow0, objectOffset),
        dot(sample.surfaceFromObjectRow1, objectOffset),
        dot(sample.surfaceFromObjectRow2, objectOffset));

    return float3(sample.uv.x + local.x * scaleU,
                  sample.uv.y + local.y * scaleV,
                  0.5F + local.z);
}

float DashrMapHeight(float normalizedHeight,
                     float heightScale,
                     float heightReferencePlane,
                     float heightOffset) {
    return 0.5F +
           (saturate(normalizedHeight) - heightReferencePlane) * heightScale +
           heightOffset;
}

float2 DashrMeasureDistortion(DashrSurfaceSampleGpu center,
                              float3 positiveUAnchor,
                              float3 negativeUAnchor,
                              float3 positiveVAnchor,
                              float3 negativeVAnchor,
                              float texelStep) {
    const float safeStep = max(texelStep, 1.0e-7F);
    const float3 deltaU = positiveUAnchor - negativeUAnchor;
    const float3 deltaV = positiveVAnchor - negativeVAnchor;
    const float denominator = 2.0F * safeStep;
    return float2(
        dot(center.surfaceFromObjectRow0, deltaU) / denominator,
        dot(center.surfaceFromObjectRow1, deltaV) / denominator);
}

bool DashrOutsideEnvelope(float3 surfacePosition,
                          float mappedMinimumHeight,
                          float mappedMaximumHeight,
                          float padding) {
    const float low = min(mappedMinimumHeight, mappedMaximumHeight) - padding;
    const float high = max(mappedMinimumHeight, mappedMaximumHeight) + padding;
    return surfacePosition.x < 0.0F || surfacePosition.x > 1.0F ||
           surfacePosition.y < 0.0F || surfacePosition.y > 1.0F ||
           surfacePosition.z < low || surfacePosition.z > high;
}

#endif
