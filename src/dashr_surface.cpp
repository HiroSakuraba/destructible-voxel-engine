#include "dve/dashr_surface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-7F;

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(Float2 value) noexcept { return finite(value.x) && finite(value.y); }
[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
[[nodiscard]] Float3 add(Float3 a, Float3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] Float3 subtract(Float3 a, Float3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] Float3 multiply(Float3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
[[nodiscard]] float dot(Float3 a, Float3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] Float3 normalized(Float3 value) noexcept {
    const float lengthSquared = dot(value, value);
    if (!(lengthSquared > kEpsilon * kEpsilon) || !finite(lengthSquared)) return {};
    return multiply(value, 1.0F / std::sqrt(lengthSquared));
}
[[nodiscard]] Float3 transform_rows(const DashrSurfaceSample& sample, Float3 value) noexcept {
    return {
        dot(sample.surfaceFromObjectRow0, value),
        dot(sample.surfaceFromObjectRow1, value),
        dot(sample.surfaceFromObjectRow2, value),
    };
}
[[nodiscard]] float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}
[[nodiscard]] bool inside_unit_uv(Float3 surface) noexcept {
    return finite(surface) &&
           surface.x >= 0.0F && surface.x <= 1.0F &&
           surface.y >= 0.0F && surface.y <= 1.0F;
}
[[nodiscard]] float sampled_height(const DashrHeightSampleFunction& function, Float2 uv) noexcept {
    if (!function) return std::numeric_limits<float>::quiet_NaN();
    const float height = function(uv);
    return finite(height) ? saturate(height) : std::numeric_limits<float>::quiet_NaN();
}
[[nodiscard]] float axis_scale(float ratio, const DashrSurfaceSettings& settings,
                               float& stepFactor) noexcept {
    if (!finite(ratio) || ratio <= settings.compressionThreshold) {
        stepFactor = std::min(stepFactor, settings.minimumStepFactor);
        return 1.0F;
    }
    if (ratio > settings.stretchThreshold) {
        const float scale = 1.0F / std::max(
            settings.stretchDamping * ratio, kEpsilon);
        stepFactor = std::min(stepFactor, std::clamp(
            scale, settings.minimumStepFactor, 1.0F));
        return scale;
    }
    return 1.0F;
}

struct TracePoint {
    Float3 object{};
    Float3 surface{};
    float distance{};
    float delta{};
    float height{};
    bool valid{};
};

[[nodiscard]] TracePoint evaluate_point(
    Float3 start,
    Float3 direction,
    float distance,
    Float2 seedUv,
    const DashrSurfaceSettings& settings,
    const DashrSurfaceSampleFunction& sampleSurface,
    const DashrHeightSampleFunction& sampleHeight,
    float* stepFactor) {
    TracePoint point;
    point.distance = distance;
    point.object = add(start, multiply(direction, distance));
    const auto field = sampleSurface ? sampleSurface(seedUv) : std::nullopt;
    if (!field || !field->valid) return point;
    float localStepFactor = 1.0F;
    point.surface = dashr_object_to_surface(*field, point.object, settings, &localStepFactor);
    if (stepFactor) *stepFactor = localStepFactor;
    if (!inside_unit_uv(point.surface)) return point;
    const float rawHeight = sampled_height(sampleHeight, {point.surface.x, point.surface.y});
    if (!finite(rawHeight)) return point;
    point.height = dashr_map_height(rawHeight, settings);
    point.delta = point.height - point.surface.z;
    point.valid = finite(point.delta);
    return point;
}

} // namespace

bool validate_dashr_surface_settings(
    const DashrSurfaceSettings& settings,
    std::string* error) noexcept {
    const auto fail = [&](const char* message) noexcept {
        if (error) *error = message;
        return false;
    };
    if (!finite(settings.heightScale) || settings.heightScale < 0.0F ||
        settings.heightScale > 4.0F ||
        !finite(settings.heightReferencePlane) ||
        settings.heightReferencePlane < 0.0F || settings.heightReferencePlane > 1.0F ||
        !finite(settings.heightOffset) ||
        !finite(settings.envelopePadding) || settings.envelopePadding < 0.0F ||
        settings.envelopePadding > 2.0F) {
        return fail("DASHR height envelope settings are invalid");
    }
    if (!finite(settings.stepSize) || !(settings.stepSize > 0.0F) ||
        !finite(settings.stepScale) || settings.stepScale < 0.0F ||
        settings.stepScale > 1.0e6F) {
        return fail("DASHR ray-step settings are invalid");
    }
    if (!finite(settings.compressionThreshold) ||
        !finite(settings.stretchThreshold) ||
        settings.stretchThreshold <= settings.compressionThreshold ||
        !finite(settings.stretchDamping) || !(settings.stretchDamping > 0.0F) ||
        !finite(settings.minimumStepFactor) ||
        !(settings.minimumStepFactor > 0.0F) || settings.minimumStepFactor > 1.0F) {
        return fail("DASHR distortion damping settings are invalid");
    }
    if (settings.maximumSteps == 0U || settings.maximumSteps > 4096U ||
        settings.refinementSteps > 16U ||
        settings.maximumTeleports > 64U) {
        return fail("DASHR iteration limits are invalid");
    }
    return true;
}

std::optional<DashrSurfaceSample> make_dashr_surface_sample(
    Float2 uv,
    Float3 objectPosition,
    Float3 tangentObject,
    Float3 bitangentObject,
    Float3 normalObject,
    Float2 distortionRatio) noexcept {
    if (!finite(uv) || !finite(objectPosition) || !finite(tangentObject) ||
        !finite(bitangentObject) || !finite(normalObject) || !finite(distortionRatio)) {
        return std::nullopt;
    }

    const float a00 = tangentObject.x,   a01 = bitangentObject.x, a02 = normalObject.x;
    const float a10 = tangentObject.y,   a11 = bitangentObject.y, a12 = normalObject.y;
    const float a20 = tangentObject.z,   a21 = bitangentObject.z, a22 = normalObject.z;
    const float determinant =
        a00 * (a11 * a22 - a12 * a21) -
        a01 * (a10 * a22 - a12 * a20) +
        a02 * (a10 * a21 - a11 * a20);
    if (!finite(determinant) || std::abs(determinant) <= kEpsilon) return std::nullopt;
    const float inverse = 1.0F / determinant;

    DashrSurfaceSample result;
    result.uv = uv;
    result.objectAnchor = objectPosition;
    result.surfaceFromObjectRow0 = {
        (a11 * a22 - a12 * a21) * inverse,
        (a02 * a21 - a01 * a22) * inverse,
        (a01 * a12 - a02 * a11) * inverse,
    };
    result.surfaceFromObjectRow1 = {
        (a12 * a20 - a10 * a22) * inverse,
        (a00 * a22 - a02 * a20) * inverse,
        (a02 * a10 - a00 * a12) * inverse,
    };
    result.surfaceFromObjectRow2 = {
        (a10 * a21 - a11 * a20) * inverse,
        (a01 * a20 - a00 * a21) * inverse,
        (a00 * a11 - a01 * a10) * inverse,
    };
    result.distortionRatio = distortionRatio;
    result.valid = true;
    return result;
}

Float2 measure_dashr_distortion(
    const DashrSurfaceSample& center,
    const DashrSurfaceSample& positiveU,
    const DashrSurfaceSample& negativeU,
    const DashrSurfaceSample& positiveV,
    const DashrSurfaceSample& negativeV,
    float texelStep) noexcept {
    if (!center.valid || !positiveU.valid || !negativeU.valid ||
        !positiveV.valid || !negativeV.valid ||
        !finite(texelStep) || !(texelStep > kEpsilon)) {
        return {1.0F, 1.0F};
    }
    const Float3 objectDeltaU = subtract(positiveU.objectAnchor, negativeU.objectAnchor);
    const Float3 objectDeltaV = subtract(positiveV.objectAnchor, negativeV.objectAnchor);
    const Float3 surfaceDeltaU = transform_rows(center, objectDeltaU);
    const Float3 surfaceDeltaV = transform_rows(center, objectDeltaV);
    const float denominator = 2.0F * texelStep;
    const float u = surfaceDeltaU.x / denominator;
    const float v = surfaceDeltaV.y / denominator;
    return {
        finite(u) ? u : 1.0F,
        finite(v) ? v : 1.0F,
    };
}

Float3 dashr_object_to_surface(
    const DashrSurfaceSample& sample,
    Float3 objectPosition,
    const DashrSurfaceSettings& settings,
    float* stepFactor) noexcept {
    float factor = 1.0F;
    if (!sample.valid || !finite(objectPosition)) {
        if (stepFactor) *stepFactor = settings.minimumStepFactor;
        return {std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
    }
    const float scaleU = axis_scale(sample.distortionRatio.x, settings, factor);
    const float scaleV = axis_scale(sample.distortionRatio.y, settings, factor);
    const Float3 local = transform_rows(sample, subtract(objectPosition, sample.objectAnchor));
    if (stepFactor) *stepFactor = factor;
    return {
        sample.uv.x + local.x * scaleU,
        sample.uv.y + local.y * scaleV,
        0.5F + local.z,
    };
}

float dashr_map_height(
    float normalizedHeight,
    const DashrSurfaceSettings& settings) noexcept {
    const float height = finite(normalizedHeight) ? saturate(normalizedHeight)
                                                  : settings.heightReferencePlane;
    return 0.5F +
           (height - settings.heightReferencePlane) * settings.heightScale +
           settings.heightOffset;
}

DashrTraceResult trace_dashr_heightfield(
    Float3 startObject,
    Float3 directionObject,
    Float2 initialUv,
    const DashrSurfaceSettings& settings,
    const DashrSurfaceSampleFunction& sampleSurface,
    const DashrHeightSampleFunction& sampleHeight,
    const DashrTeleportSampleFunction& sampleTeleport) {
    DashrTraceResult result;
    std::string ignored;
    if (!validate_dashr_surface_settings(settings, &ignored) ||
        !finite(startObject) || !finite(directionObject) || !finite(initialUv) ||
        !sampleSurface || !sampleHeight) {
        result.status = DashrTraceStatus::InvalidField;
        return result;
    }
    const Float3 direction = normalized(directionObject);
    if (dot(direction, direction) <= kEpsilon) {
        result.status = DashrTraceStatus::InvalidField;
        return result;
    }

    const float mapped0 = dashr_map_height(0.0F, settings);
    const float mapped1 = dashr_map_height(1.0F, settings);
    const float envelopeMin = std::min(mapped0, mapped1) - settings.envelopePadding;
    const float envelopeMax = std::max(mapped0, mapped1) + settings.envelopePadding;

    Float2 seedUv = initialUv;
    float distance = 0.0F;
    float nextStep = 0.0F;
    TracePoint previous{};
    bool havePreviousOutside = false;
    bool teleportedThisStep = false;
    // A destination texel can intentionally be inset only a few pixels from the
    // paired edge. Filtering may therefore still report a positive seam region
    // after teleporting. Keep traversing that destination island until the ray
    // has actually exited its seam band instead of bouncing A<->B.
    bool teleportCooldown = false;

    for (std::uint32_t iteration = 0U; iteration < settings.maximumSteps; ++iteration) {
        teleportedThisStep = false;

        if (sampleTeleport) {
            const auto teleport = sampleTeleport(seedUv);
            if (!teleport || !teleport->valid || !finite(teleport->signedDistance) ||
                !finite(teleport->destinationUv)) {
                result.status = DashrTraceStatus::InvalidField;
                return result;
            }
            if (teleportCooldown) {
                if (teleport->signedDistance <= 0.0F) teleportCooldown = false;
            } else if (teleport->signedDistance > 0.0F) {
                if (result.teleports >= settings.maximumTeleports) {
                    result.status = DashrTraceStatus::TeleportLimit;
                    return result;
                }
                seedUv = teleport->destinationUv;
                ++result.teleports;
                teleportedThisStep = true;
                teleportCooldown = true;
            }
        }

        const float previousDistance = distance;
        distance += nextStep;
        float stepFactor = 1.0F;
        TracePoint current = evaluate_point(
            startObject, direction, distance, seedUv, settings,
            sampleSurface, sampleHeight, &stepFactor);
        if (!current.valid) {
            const auto field = sampleSurface(seedUv);
            if (!field || !field->valid) {
                result.status = DashrTraceStatus::InvalidField;
            } else {
                result.status = DashrTraceStatus::Escaped;
            }
            result.objectDistance = distance;
            result.objectPosition = current.object;
            result.surfacePosition = current.surface;
            return result;
        }

        if (stepFactor < 1.0F && nextStep > 0.0F) {
            distance = previousDistance + nextStep * std::clamp(
                stepFactor, settings.minimumStepFactor, 1.0F);
            current = evaluate_point(
                startObject, direction, distance, seedUv, settings,
                sampleSurface, sampleHeight, nullptr);
            if (!current.valid) {
                result.status = DashrTraceStatus::Escaped;
                result.objectDistance = distance;
                result.objectPosition = current.object;
                result.surfacePosition = current.surface;
                return result;
            }
        }

        if (current.surface.z < envelopeMin || current.surface.z > envelopeMax) {
            result.status = DashrTraceStatus::Escaped;
            result.objectDistance = current.distance;
            result.objectPosition = current.object;
            result.surfacePosition = current.surface;
            result.sampledHeight = current.height;
            result.steps = iteration + 1U;
            return result;
        }

        ++result.steps;
        if (current.delta >= 0.0F) {
            TracePoint hit = current;
            if (havePreviousOutside && previous.delta < 0.0F &&
                !teleportedThisStep && settings.refinementSteps > 0U) {
                TracePoint low = previous;
                TracePoint high = current;
                for (std::uint32_t refine = 0U; refine < settings.refinementSteps; ++refine) {
                    const float middleDistance = (low.distance + high.distance) * 0.5F;
                    const Float2 middleSeed{
                        (low.surface.x + high.surface.x) * 0.5F,
                        (low.surface.y + high.surface.y) * 0.5F,
                    };
                    TracePoint middle = evaluate_point(
                        startObject, direction, middleDistance, middleSeed, settings,
                        sampleSurface, sampleHeight, nullptr);
                    if (!middle.valid) break;
                    ++result.refinementSteps;
                    if (middle.delta >= 0.0F) high = middle;
                    else low = middle;
                }
                hit = high;
            }
            result.status = DashrTraceStatus::Hit;
            result.objectDistance = hit.distance;
            result.objectPosition = hit.object;
            result.surfacePosition = hit.surface;
            result.sampledHeight = hit.height;
            return result;
        }

        previous = current;
        havePreviousOutside = true;
        seedUv = {current.surface.x, current.surface.y};
        nextStep = settings.stepSize *
                   std::max(1.0F, -current.delta * settings.stepScale);
    }

    result.status = DashrTraceStatus::StepLimit;
    result.objectDistance = distance;
    result.objectPosition = add(startObject, multiply(direction, distance));
    return result;
}

} // namespace dve
