#include "common/camera_cinematic.hlsli"

cbuffer CinematicDofConstants : register(b0) {
    uint gImageWidth;
    uint gImageHeight;
    uint gCameraPacketIndex;
    uint gSampleCount;
    float gMaximumBlurRadiusFraction; // fraction of image height, so the look is resolution independent
    uint gPreserveForegroundEdges;
    uint gFrameIndex;
    uint gReserved0;
};

StructuredBuffer<CameraGpuPacket> gCameraPackets : register(t0);
StructuredBuffer<float4> gInputColor : register(t1);
StructuredBuffer<float> gLinearDepth : register(t2);
RWStructuredBuffer<float4> gOutputColor : register(u0);

uint PixelIndex(uint2 pixel) { return pixel.y * gImageWidth + pixel.x; }

float MaximumBlurRadiusPixels() {
    return max(0.0F, gMaximumBlurRadiusFraction) * float(gImageHeight);
}

float2 ScreenFromPixel(uint2 pixel) {
    return (float2(pixel) + 0.5F.xx) / float2(gImageWidth, gImageHeight) * 2.0F - 1.0F.xx;
}

// The aperture is sampled as a unit disk. `jitter` rotates the whole Vogel spiral per pixel so
// that undersampling reads as dither rather than as one fixed ring pattern repeated everywhere.
float2 ApertureSample(uint sampleIndex, uint sampleCount, CameraGpuPacket camera, float2 screen,
                      float jitter) {
    const float goldenAngle = 2.39996323F;
    const float twoPi = 6.28318530718F;
    float radius = sqrt((float(sampleIndex) + 0.5F) / float(max(1U, sampleCount)));
    float angle = float(sampleIndex) * goldenAngle + jitter;
    uint blades = uint(camera.bokeh.x + 0.5F);
    if (blades >= 3U) {
        // The aperture polygon has to rotate with bladeRotation, so its boundary is evaluated in
        // the aperture's own frame (angle - rotation). Evaluating it at `angle`, as before, made
        // the polygon orientation independent of the control and the rotation a no-op.
        float sector = twoPi / float(blades);
        float local = angle - camera.bokeh.y;
        // HLSL fmod keeps the sign of the dividend, so a negative blade rotation used to push
        // `local` outside the intended half sector and let cos(local) go negative.
        local = local - floor(local / sector + 0.5F) * sector;
        float polygonRadius = cos(3.14159265359F / float(blades)) / max(0.15F, cos(local));
        radius *= lerp(polygonRadius, 1.0F, saturate(camera.bokeh.z));
    }
    // Anamorphic highlights are elongated along the vertical axis in the projected image, so the
    // squeeze goes on x and the stretch on y. The sqrt split keeps sampled disk area constant, so
    // the ratio changes bokeh shape without changing total blur energy.
    float ratio = sqrt(max(0.25F, camera.bokeh.w));
    float2 result = float2(cos(angle) * radius / ratio, sin(angle) * radius * ratio);

    // Optical vignetting (cat's eye): the rear of the barrel clips the aperture toward frame
    // centre, truncating the disk into a lens shape and darkening the corner. It does not shrink
    // the blur circle isotropically, which is what the previous scale factor did (it made corners
    // sharper instead of lens shaped).
    float catEye = saturate(camera.splitDiopter1.w);
    if (catEye > 0.0F) {
        float screenLength = length(screen);
        float edge = saturate(screenLength / 1.41421356F);
        float2 towardCentre = screenLength > 1.0e-5F ? -screen / screenLength : float2(0.0F, 0.0F);
        // A tap survives only where the aperture disk and the displaced clipping circle overlap.
        if (length(result - towardCentre * catEye * edge) > 1.0F) return float2(0.0F, 0.0F);
    }
    return result;
}

float EffectiveSensorHeightMillimeters(CameraGpuPacket camera) {
    // Must match CameraPhysicalLens::vertical_field_of_view_radians, otherwise the blur does not
    // agree with the rendered field of view for any gate fit other than Vertical.
    bool physical = camera.lens.w > 0.5F;
    float sensorHeight = physical ? max(0.001F, camera.physicalLens0.z) : 24.0F;
    float sensorWidth = physical ? max(0.001F, camera.physicalLens0.y) : 36.0F;
    float aspect = float(gImageWidth) / max(1.0F, float(gImageHeight));
    float fitted = sensorWidth / max(0.01F, aspect);
    uint gateFit = uint(camera.physicalLens1.y + 0.5F);
    if (gateFit == 1U) return fitted;                      // Horizontal
    if (gateFit == 2U) return min(sensorHeight, fitted);   // Fill
    if (gateFit == 3U) return max(sensorHeight, fitted);   // Overscan
    return sensorHeight;                                   // Vertical, Stretch
}

float CircleOfConfusion(float depth, float focus, CameraGpuPacket camera) {
    if (!(depth > 0.0F)) return 0.0F;
    bool physical = camera.lens.w > 0.5F;
    float focal = (physical ? max(1.0F, camera.physicalLens0.x) : 35.0F) * 0.001F;
    float aperture = max(0.5F, camera.postProcess1.w);
    focus = max(focal + 0.001F, focus);
    float apertureDiameter = focal / aperture;
    float blurMeters = abs(apertureDiameter * focal * (depth - focus) /
                           max(1.0e-6F, depth * (focus - focal)));
    float sensorHeight = EffectiveSensorHeightMillimeters(camera) * 0.001F;
    float weight = saturate(camera.postProcess1.y); // clamped to match the CPU reference
    return min(MaximumBlurRadiusPixels(),
               0.5F * blurMeters / sensorHeight * float(gImageHeight) * weight);
}

// A split diopter is a half lens cemented over one side of the front element. It gives two focus
// planes with a seam between them where both optical paths overlap and neither is in focus, so the
// seam is the blurriest region in frame. Interpolating the focus distance across the seam, as
// before, did the opposite: it swept focus through the intermediate distances and brought
// mid-distance subjects into focus exactly on the line.
float SplitDiopterCoc(uint2 pixel, float depth, CameraGpuPacket camera) {
    if (camera.splitDiopter0.x < 0.5F) {
        return CircleOfConfusion(depth, camera.postProcess1.z, camera);
    }
    float2 screen = ScreenFromPixel(pixel);
    float2 normal = float2(cos(camera.splitDiopter1.z), sin(camera.splitDiopter1.z));
    float signedDistance = dot(screen - camera.splitDiopter1.xy, normal);
    float feather = max(1.0e-4F, camera.splitDiopter0.w);
    float blend = smoothstep(-feather, feather, signedDistance);
    float nearCoc = CircleOfConfusion(depth, camera.splitDiopter0.y, camera);
    float farCoc = CircleOfConfusion(depth, camera.splitDiopter0.z, camera);
    // seam is 0 on either clean half and 1 at the centre of the transition band.
    float seam = 1.0F - abs(2.0F * blend - 1.0F);
    return lerp(lerp(nearCoc, farCoc, blend), max(nearCoc, farCoc), seam);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gImageWidth || pixel.y >= gImageHeight) return;
    CameraGpuPacket camera = gCameraPackets[gCameraPacketIndex];
    uint index = PixelIndex(pixel);
    float depth = gLinearDepth[index];
    float radius = SplitDiopterCoc(pixel, depth, camera);

    // A sharp pixel can still be covered by a nearer, blurrier neighbour, so the gather radius
    // cannot be the centre pixel's own circle of confusion. Without this, out of focus foreground
    // never spreads over a sharp background, which is the most conspicuous depth of field error.
    // The coarse probe ring stands in for the tile-max pass a production implementation would use.
    float gatherRadius = radius;
    float probeRadius = MaximumBlurRadiusPixels();
    if (probeRadius > radius) {
        for (uint probe = 0U; probe < 8U; ++probe) {
            float probeAngle = float(probe) * 0.78539816F;
            int2 probePixel = clamp(
                int2(round(float2(pixel) + float2(cos(probeAngle), sin(probeAngle)) * probeRadius)),
                int2(0, 0), int2(int(gImageWidth) - 1, int(gImageHeight) - 1));
            uint probeIndex = PixelIndex(uint2(probePixel));
            float probeDepth = gLinearDepth[probeIndex];
            if (probeDepth < depth) {
                gatherRadius = max(gatherRadius,
                                   SplitDiopterCoc(uint2(probePixel), probeDepth, camera));
            }
        }
    }

    if (gatherRadius < 0.5F || gSampleCount == 0U) {
        gOutputColor[index] = gInputColor[index];
        return;
    }

    float2 screen = ScreenFromPixel(pixel);
    float jitter = camera.bokeh.y + HashNoise(pixel, gFrameIndex, 91U) * 6.28318530718F;
    float4 sum = gInputColor[index];
    float weight = 1.0F;
    uint count = min(gSampleCount, 32U);
    for (uint sample = 0U; sample < count; ++sample) {
        float2 unit = ApertureSample(sample, count, camera, screen, jitter);
        if (dot(unit, unit) <= 0.0F) continue; // clipped by optical vignetting
        float2 offset = unit * gatherRadius;
        // 2026-07-25: measure the reach test in the aperture's own metric. The circle of confusion
        // is the radius of a circular aperture image; the anamorphic aperture is an ellipse of the
        // same area with semi-axes coc/ratio and coc*ratio, so a tap on the vertical extreme is
        // ratio^2 further away in pixels while being equally inside the aperture. Comparing raw
        // pixel distance against the circular coc rejected precisely the taps carrying the
        // anamorphic shape and clipped the bokeh back to a circle. Must match the CPU reference in
        // camera_frame_graph.cpp.
        float shapeRatio = sqrt(max(0.25F, camera.bokeh.w));
        float2 shaped = float2(offset.x * shapeRatio, offset.y / shapeRatio);
        float offsetLength = length(shaped);
        int2 source = clamp(int2(round(float2(pixel) + offset)), int2(0, 0),
                            int2(int(gImageWidth) - 1, int(gImageHeight) - 1));
        uint sourceIndex = PixelIndex(uint2(source));
        float sourceDepth = gLinearDepth[sourceIndex];
        float sourceCoc = SplitDiopterCoc(uint2(source), sourceDepth, camera);

        // Scatter as gather: a tap contributes only where its own blur circle actually reaches
        // this pixel. Background taps additionally contribute out to this pixel's own circle of
        // confusion, which is the ordinary gather assumption for defocused background.
        float reach = sourceCoc;
        if (sourceDepth >= depth) reach = max(reach, radius);
        float sampleWeight = saturate(reach - offsetLength + 1.0F);
        if (gPreserveForegroundEdges != 0U && sourceDepth + 0.05F < depth) {
            // Nearer geometry may only bleed in as far as it is genuinely blurred.
            sampleWeight = min(sampleWeight, saturate(sourceCoc - offsetLength + 1.0F));
        }
        if (sampleWeight <= 0.0F) continue;
        sum += gInputColor[sourceIndex] * sampleWeight;
        weight += sampleWeight;
    }
    gOutputColor[index] = sum / max(weight, 1.0e-5F);
}
