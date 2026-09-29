#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "dve/asset_cooker.hpp"

namespace dve {

// DVE port of the surface-space heightfield tracing idea described by Tom Forsyth's
// DASHR (Dynamically Animated Skinned Heightfield Rendering). This is a clean DVE
// implementation of the technique, not the original DirectX demo.
//
// The surface sample stores the stable "DistortionMode 1" representation: an
// object-space anchor for (u,v,0.5) and the inverse, generally non-orthonormal,
// tangent/bitangent/normal basis needed to map nearby object-space points back to
// surface space.
struct DashrSurfaceSample {
    Float2 uv{};
    Float3 objectAnchor{};
    Float3 surfaceFromObjectRow0{};
    Float3 surfaceFromObjectRow1{};
    Float3 surfaceFromObjectRow2{};
    Float2 distortionRatio{1.0F, 1.0F};
    bool valid{true};
};

struct DashrTeleportSample {
    Float2 destinationUv{};
    // Positive means the current point is inside the seam-teleport region.
    // Negative means ordinary surface space. A signed-distance texture can
    // therefore be filtered independently from the point-sampled destination.
    float signedDistance{-1.0F};
    bool valid{true};
};

struct DashrSurfaceSettings {
    // Height values are interpreted around heightReferencePlane in surface-space
    // height units. A value of 1 means one object-space unit along the supplied
    // normal basis; production material code will normally use a much smaller value.
    float heightScale{0.04F};
    float heightReferencePlane{0.5F};
    float heightOffset{};

    // Extra surface-space shell around the authored height range. The actual
    // rasterized shell can use a separate conservative extrusion.
    float envelopePadding{0.02F};

    // Base object-space step and distance-to-surface acceleration factor.
    float stepSize{0.005F};
    float stepScale{16.0F};

    // Distortion damping. Ratios are one on an undeformed parameterization.
    // Near-zero/negative ratios indicate compression or inversion; large ratios
    // indicate stretch.
    float compressionThreshold{0.05F};
    float stretchThreshold{1.5F};
    float stretchDamping{1.0F};
    float minimumStepFactor{0.01F};

    std::uint32_t maximumSteps{192U};
    std::uint32_t refinementSteps{4U};
    std::uint32_t maximumTeleports{8U};
};

enum class DashrTraceStatus : std::uint8_t {
    Hit,
    Escaped,
    InvalidField,
    StepLimit,
    TeleportLimit,
};

struct DashrTraceResult {
    DashrTraceStatus status{DashrTraceStatus::InvalidField};
    Float3 objectPosition{};
    Float3 surfacePosition{};
    float objectDistance{};
    float sampledHeight{};
    std::uint32_t steps{};
    std::uint32_t refinementSteps{};
    std::uint32_t teleports{};

    [[nodiscard]] bool hit() const noexcept { return status == DashrTraceStatus::Hit; }
};

using DashrSurfaceSampleFunction =
    std::function<std::optional<DashrSurfaceSample>(Float2)>;
using DashrHeightSampleFunction = std::function<float(Float2)>;
using DashrTeleportSampleFunction =
    std::function<std::optional<DashrTeleportSample>(Float2)>;

[[nodiscard]] bool validate_dashr_surface_settings(
    const DashrSurfaceSettings& settings,
    std::string* error = nullptr) noexcept;

// Build an object->surface differential frame from a deformed object-space
// position and non-normalized surface basis. Keeping tangent/bitangent scale is
// essential: DASHR relies on the local differential, not only basis direction.
[[nodiscard]] std::optional<DashrSurfaceSample> make_dashr_surface_sample(
    Float2 uv,
    Float3 objectPosition,
    Float3 tangentObject,
    Float3 bitangentObject,
    Float3 normalObject,
    Float2 distortionRatio = {1.0F, 1.0F}) noexcept;

// Estimate U/V animation distortion from neighboring field samples. Unlike the
// original proof-of-concept's heuristic finite difference, this central
// derivative is normalized by the full 2*texelStep so an identity mapping
// evaluates to approximately (1,1).
[[nodiscard]] Float2 measure_dashr_distortion(
    const DashrSurfaceSample& center,
    const DashrSurfaceSample& positiveU,
    const DashrSurfaceSample& negativeU,
    const DashrSurfaceSample& positiveV,
    const DashrSurfaceSample& negativeV,
    float texelStep) noexcept;

// Convert an object-space position into local (u,v,height) surface space using
// the last known surface sample as the lookup seed. stepFactor reports how much
// an object-space ray step should be shortened in distorted areas.
[[nodiscard]] Float3 dashr_object_to_surface(
    const DashrSurfaceSample& sample,
    Float3 objectPosition,
    const DashrSurfaceSettings& settings,
    float* stepFactor = nullptr) noexcept;

[[nodiscard]] float dashr_map_height(
    float normalizedHeight,
    const DashrSurfaceSettings& settings) noexcept;

// Bounded CPU/reference tracer. The GPU implementation should preserve this
// contract while replacing callbacks with textures/atlases.
[[nodiscard]] DashrTraceResult trace_dashr_heightfield(
    Float3 startObject,
    Float3 directionObject,
    Float2 initialUv,
    const DashrSurfaceSettings& settings,
    const DashrSurfaceSampleFunction& sampleSurface,
    const DashrHeightSampleFunction& sampleHeight,
    const DashrTeleportSampleFunction& sampleTeleport = {});

} // namespace dve
