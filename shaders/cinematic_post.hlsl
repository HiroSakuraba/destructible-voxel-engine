#include "common/camera_cinematic.hlsli"

cbuffer CinematicPostConstants : register(b0) {
    uint gImageWidth;
    uint gImageHeight;
    uint gCameraPacketIndex;
    uint gFrameIndex;
    uint gApplyLut;
    uint gApplyMotionBlur;
    uint gMaximumMotionSamples;
    uint gColorLutEdgeSize;   // needed for half texel correction; 0 disables the LUT
    uint gApplyFilmEffects;   // halation, anamorphic flare, sharpen, grain
    uint gReserved0;
    uint gReserved1;
    uint gReserved2;
};

StructuredBuffer<CameraGpuPacket> gCameraPackets : register(t0);
StructuredBuffer<float4> gInputColor : register(t1);
StructuredBuffer<float2> gMotionPixels : register(t2);
Texture3D<float4> gColorLut : register(t3);
SamplerState gColorLutSampler : register(s0);
RWStructuredBuffer<float4> gOutputColor : register(u0);

uint PixelIndex(uint2 pixel) {
    return pixel.y * gImageWidth + pixel.x;
}

float4 LoadColor(int2 pixel) {
    pixel = clamp(pixel, int2(0, 0), int2(int(gImageWidth) - 1, int(gImageHeight) - 1));
    return gInputColor[PixelIndex(uint2(pixel))];
}

float4 SampleColor(float2 pixel) {
    float2 clamped = clamp(pixel, 0.0F.xx, float2(gImageWidth - 1U, gImageHeight - 1U));
    int2 p0 = int2(floor(clamped));
    int2 p1 = min(p0 + 1, int2(int(gImageWidth) - 1, int(gImageHeight) - 1));
    float2 t = clamped - float2(p0);
    float4 a = lerp(LoadColor(p0), LoadColor(int2(p1.x, p0.y)), t.x);
    float4 b = lerp(LoadColor(int2(p0.x, p1.y)), LoadColor(p1), t.x);
    return lerp(a, b, t.y);
}

float2 DistortedPixel(float2 pixel, CameraGpuPacket camera) {
    float aspect = float(gImageWidth) / max(1.0F, float(gImageHeight));
    float2 p = (pixel + 0.5F.xx) / float2(gImageWidth, gImageHeight) * 2.0F - 1.0F.xx;
    p.x *= aspect;
    // No horizontal remap for anamorphicSqueeze. There is no squeezed intermediate image to
    // undo: the squeeze widens the frustum in camera_projection_matrix, so what arrives here is
    // already desqueezed. Dividing x here magnified the centre of frame instead.
    // Focus breathing is a magnification change, m = f / (D - f), referenced to infinity focus.
    // The previous saturate(1/D) pinned the effect at its maximum for every focus distance below
    // one metre, which is exactly the range where breathing is visible.
    float focalMeters = (camera.lens.w > 0.5F ? max(1.0F, camera.physicalLens0.x) : 35.0F) * 0.001F;
    float focusMeters = max(focalMeters + 0.001F, camera.postProcess1.z);
    float magnification = focalMeters / (focusMeters - focalMeters);
    p *= 1.0F + camera.lensEffects1.w * magnification;
    float radius = length(p);
    uint model = uint(camera.lensEffects0.x + 0.5F);
    if (model == 1U) {
        float r2 = radius * radius;
        float scale = 1.0F + camera.lensEffects0.y * r2 +
                      camera.lensEffects0.z * r2 * r2 +
                      camera.lensEffects0.w * r2 * r2 * r2;
        p *= scale;
    } else if (model == 2U && camera.lensEffects1.x > 0.0F && radius > 1.0e-6F) {
        float normalizedRadius = min(1.0F, radius / 1.41421356F);
        float maximumAngle = 0.45F + camera.lensEffects1.x * 0.85F;
        float mapped = tan(normalizedRadius * maximumAngle) / tan(maximumAngle) * 1.41421356F;
        p *= mapped / radius;
    }
    float weaveX = (HashNoise(uint2(0U, 0U), gFrameIndex, 17U) - 0.5F) * camera.lensEffects2.z * 2.0F;
    float weaveY = (HashNoise(uint2(1U, 0U), gFrameIndex, 31U) - 0.5F) * camera.lensEffects2.z * 2.0F;
    return float2((p.x / aspect + 1.0F) * 0.5F * gImageWidth - 0.5F + weaveX,
                  (p.y + 1.0F) * 0.5F * gImageHeight - 0.5F + weaveY);
}

// Lateral chromatic aberration grows with field height and is zero on the optical axis. The
// previous constant offset displaced the channels even at the exact centre of frame.
float3 FetchAberrated(float2 sourcePixel, float aberrationPixels) {
    float2 centre = float2(gImageWidth, gImageHeight) * 0.5F;
    float2 delta = sourcePixel - centre;
    float distance = length(delta);
    float4 base = SampleColor(sourcePixel);
    if (!(aberrationPixels > 0.0F) || distance < 1.0e-4F) return base.rgb;
    float2 radial = delta / distance;
    float fieldHeight = saturate(distance / max(1.0F, length(centre)));
    float2 shift = radial * aberrationPixels * fieldHeight;
    return float3(SampleColor(sourcePixel + shift).r, base.g, SampleColor(sourcePixel - shift).b);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gImageWidth || pixel.y >= gImageHeight) return;
    CameraGpuPacket camera = gCameraPackets[gCameraPacketIndex];
    float2 sourcePixel = DistortedPixel(float2(pixel), camera);
    float aberration = camera.lensEffects1.y;
    float4 base = SampleColor(sourcePixel);
    float3 color = FetchAberrated(sourcePixel, aberration);

    if (gApplyMotionBlur != 0U && camera.film0.w > 0.0F) {
        float2 motion = gMotionPixels[PixelIndex(pixel)] * saturate(camera.film0.w) *
                        (clamp(camera.film1.x, 0.0F, 360.0F) / 360.0F);
        uint sampleCount = clamp(uint(ceil(length(motion))) + 1U, 2U, max(2U, gMaximumMotionSamples));
        float3 sum = 0.0F.xxx;
        for (uint sample = 0U; sample < sampleCount; ++sample) {
            float t = float(sample) / float(sampleCount - 1U) - 0.5F;
            // Aberration is applied to each motion tap. Overwriting `color` with an unaberrated
            // accumulation, as before, silently removed chromatic aberration whenever motion blur
            // was enabled.
            sum += FetchAberrated(sourcePixel + motion * t, aberration);
        }
        color = sum / float(sampleCount);
    }

    // Exposure has to come before any threshold based effect, otherwise what blooms or halates
    // does not change when the camera is stopped down.
    color *= camera.postProcess0.x;

    if (gApplyFilmEffects != 0U) {
        float exposure = camera.postProcess0.x;
        float sharpen = camera.film0.z;
        if (sharpen > 0.0F) {
            float3 average = (FetchAberrated(sourcePixel + float2(-1.0F, 0.0F), aberration) +
                              FetchAberrated(sourcePixel + float2(1.0F, 0.0F), aberration) +
                              FetchAberrated(sourcePixel + float2(0.0F, -1.0F), aberration) +
                              FetchAberrated(sourcePixel + float2(0.0F, 1.0F), aberration)) * 0.25F;
            color += (color - average * exposure) * sharpen;
        }

        // Halation is light that passes through the emulsion, scatters in the base and reflects
        // off the backing. The visible result is a wide red-orange glow around clipped highlights,
        // tens of pixels across. A 3x3 gather, as before, is invisible at any delivery resolution.
        float halation = camera.film0.y;
        if (halation > 0.0F) {
            float halationRadius = 0.02F * float(gImageHeight);
            float halo = 0.0F;
            float haloWeight = 0.0F;
            for (uint ring = 1U; ring <= 2U; ++ring) {
                float ringRadius = halationRadius * float(ring) * 0.5F;
                float ringWeight = 1.0F / float(ring);
                for (uint step = 0U; step < 8U; ++step) {
                    float stepAngle = (float(step) + 0.5F * float(ring)) * 0.78539816F;
                    float2 tap = sourcePixel + float2(cos(stepAngle), sin(stepAngle)) * ringRadius;
                    float tapLuminance = Luminance(SampleColor(tap).rgb) * exposure;
                    halo += max(0.0F, tapLuminance - 1.0F) * ringWeight;
                    haloWeight += ringWeight;
                }
            }
            halo = halo / max(haloWeight, 1.0e-5F) * halation;
            color += float3(halo * 0.75F, halo * 0.20F, halo * 0.05F);
        }

        // Anamorphic flare is a long horizontal streak from the cylindrical element, spanning a
        // useful fraction of the frame width rather than the previous 25 pixels.
        float flareIntensity = camera.lensEffects2.x;
        if (flareIntensity > 0.0F) {
            float flareSpan = 0.12F * float(gImageWidth);
            float threshold = camera.lensEffects2.y;
            float flare = 0.0F;
            float flareWeight = 0.0F;
            for (uint step = 1U; step <= 12U; ++step) {
                float distance = flareSpan * float(step) / 12.0F;
                float stepWeight = 1.0F - float(step - 1U) / 12.0F;
                float left = Luminance(SampleColor(sourcePixel - float2(distance, 0.0F)).rgb) * exposure;
                float right = Luminance(SampleColor(sourcePixel + float2(distance, 0.0F)).rgb) * exposure;
                flare += (max(0.0F, left - threshold) + max(0.0F, right - threshold)) * stepWeight;
                flareWeight += 2.0F * stepWeight;
            }
            flare = flare / max(flareWeight, 1.0e-5F) * flareIntensity;
            color += float3(flare * 0.35F, flare * 0.60F, flare);
        }
    }

    // Colour temperature is perceptually uniform in mireds, not in kelvin. The sign convention is
    // unchanged: lowering the value warms the image, which is what the shipped presets expect.
    const float referenceMired = 1.0e6F / 6500.0F;
    float mired = 1.0e6F / clamp(camera.colorGrade0.x, 1000.0F, 20000.0F);
    float warmth = clamp((mired - referenceMired) / 158.7F, -1.0F, 1.0F);
    float tint = camera.colorGrade0.y;
    color *= float3(1.0F + 0.12F * warmth + 0.05F * tint,
                    1.0F - 0.08F * tint,
                    1.0F - 0.12F * warmth + 0.05F * tint);
    color += camera.colorGradeLift.rgb;
    color = pow(max(color, 0.0F.xxx), 1.0F.xxx / max(camera.colorGradeGamma.rgb, 0.05F.xxx)) *
            camera.colorGradeGain.rgb;
    color = (color - 0.18F.xxx) * camera.postProcess0.w + 0.18F.xxx;
    float luma = Luminance(color);
    color = luma.xxx + (color - luma.xxx) * camera.postProcess0.z;
    color *= camera.colorTint.rgb;

    // Vignetting is an irradiance falloff at the aperture, so it belongs in linear scene referred
    // light ahead of the tone curve. Applied after tonemapping it darkens display values instead
    // of removing light, which changes the highlight rolloff rather than dimming the corners.
    float2 uv = (float2(pixel) + 0.5F.xx) / float2(gImageWidth, gImageHeight);
    float2 centered = uv * 2.0F - 1.0F.xx;
    float vignette = 1.0F - camera.postProcess1.x * smoothstep(0.25F, 1.0F, dot(centered, centered) * 0.5F);
    color *= max(0.0F, vignette);

    color = TonemapCamera(color, uint(camera.colorGrade0.w + 0.5F));

    // Grading LUTs are authored against display referred input, so the lookup happens after the
    // tone curve. Sampling saturate(linear) before tonemapping, as before, collapsed every value
    // above 1.0 onto the LUT white corner and destroyed highlight separation.
    if (gApplyLut != 0U && gColorLutEdgeSize > 1U && camera.colorGrade0.z > 0.0F) {
        float edge = float(gColorLutEdgeSize);
        // Half texel correction: without it the lookup is biased by 0.5/N through the interior and
        // does not agree with the CPU reference, which maps colour onto (N-1) texels exactly.
        float3 coordinate = saturate(color) * ((edge - 1.0F) / edge) + (0.5F / edge).xxx;
        float3 mapped = gColorLut.SampleLevel(gColorLutSampler, coordinate, 0.0F).rgb;
        color = lerp(color, mapped, saturate(camera.colorGrade0.z));
    }

    if (gApplyFilmEffects != 0U && camera.film0.x > 0.0F) {
        // Film grain density peaks in the midtones and vanishes in clipped white and clean black.
        // Uniform additive noise, as before, lifted the blacks because the negative half of the
        // signal was clipped away by the final max().
        float grainLuma = saturate(Luminance(color));
        float density = 4.0F * grainLuma * (1.0F - grainLuma);
        color += (HashNoise(pixel, gFrameIndex, uint(camera.film1.y + 0.5F)) - 0.5F) *
                 camera.film0.x * 0.12F * density;
    }

    uint framing = uint(camera.film1.z + 0.5F);
    float nativeAspect = float(gImageWidth) / max(1.0F, float(gImageHeight));
    float targetAspect = FramingAspect(framing, camera.film1.w, nativeAspect);
    bool matte = false;
    if (nativeAspect > targetAspect) {
        float widthFraction = targetAspect / nativeAspect;
        matte = uv.x < (1.0F - widthFraction) * 0.5F || uv.x > 1.0F - (1.0F - widthFraction) * 0.5F;
    } else if (nativeAspect < targetAspect) {
        float heightFraction = nativeAspect / targetAspect;
        matte = uv.y < (1.0F - heightFraction) * 0.5F || uv.y > 1.0F - (1.0F - heightFraction) * 0.5F;
    }
    if (matte) color = lerp(color, camera.matteColor.rgb, camera.matteColor.w);
    gOutputColor[PixelIndex(pixel)] = float4(max(color, 0.0F.xxx), base.a);
}
