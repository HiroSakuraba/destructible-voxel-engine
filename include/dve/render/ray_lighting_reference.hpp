#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/render_environment.hpp"
#include "dve/types.hpp"

namespace dve::render {

struct Float2 {
    float x{};
    float y{};
};

struct LightingTemporalSettings {
    float historyWeight{0.90F};
    float depthToleranceMeters{0.05F};
    float normalThreshold{0.85F};
    std::uint32_t maximumHistory{32U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct LightingTemporalSample {
    Float3 indirectDiffuse{};
    float ambientOcclusion{1.0F};
    float shadowVisibility{1.0F};
    float depthMeters{};
    Float3 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t historyLength{};
};

[[nodiscard]] float meters_to_voxel_units(float meters, float metersPerVoxel) noexcept;
[[nodiscard]] float voxel_units_to_meters(float voxels, float metersPerVoxel) noexcept;

// RenderEnvironment's metre-authored ray distances converted to the voxel-index units the
// tracers use, exactly as the GPU does with MetersToVoxelUnits(...) and gMetersPerVoxel.
// metersPerVoxel goes through resolve_meters_per_voxel. Minimum-bias floors (e.g. the GPU's
// max(1e-3, bias)) are applied by the consumers, in voxel units, as on the GPU.
struct VoxelLightingDistances {
    float metersPerVoxel{kDefaultMetersPerVoxel};
    float globalIlluminationMaxDistance{};
    float shadowMaxDistance{};
    float contactShadowDistance{};
    float shadowBias{};
    float subsurfaceMaxDistance{};
    float ambientOcclusionMaxDistance{};
};
[[nodiscard]] VoxelLightingDistances voxel_lighting_distances(
    const RenderEnvironment& environment, float metersPerVoxel = kDefaultMetersPerVoxel) noexcept;

[[nodiscard]] float distance_weighted_ao_visibility(
    bool hit, float hitDistanceVoxels, float maximumDistanceMeters,
    float metersPerVoxel) noexcept;
[[nodiscard]] Float2 hammersley_2d(
    std::uint32_t sampleIndex, std::uint32_t sampleCount) noexcept;
[[nodiscard]] Float2 rotated_hammersley_2d(
    std::uint32_t sampleIndex, std::uint32_t sampleCount,
    Float2 rotation) noexcept;
// Shirley-Chiu concentric map, unit square -> unit disc (mirror of ConcentricSquareToDisk in
// random_sampling.hlsli): equal-area and stratification-preserving. Used for sun-disc samples.
[[nodiscard]] Float2 concentric_square_to_disk(Float2 square) noexcept;
// CPU mirrors of random_sampling.hlsli: PcgHash, Hash2, CosineWeightedHemisphereSample (with
// BuildOrthonormalBasis). Bit-for-bit the same arithmetic in the same order.
[[nodiscard]] std::uint32_t pcg_hash_32(std::uint32_t input) noexcept;
[[nodiscard]] Float2 hash_2d(std::uint32_t seed) noexcept;
[[nodiscard]] Float3 cosine_weighted_hemisphere_sample(Float3 normal, Float2 uniformRandom) noexcept;

// Ambient occlusion: see dve/render/ambient_occlusion.hpp.

[[nodiscard]] Float3 one_bounce_diffuse_radiance(
    Float3 baseColor, float metallic, Float3 emissive, Float3 ambientRadiance,
    Float3 sunColor, float sunIntensity, Float3 bounceNormal,
    Float3 sunDirection, float sunVisibility) noexcept;
[[nodiscard]] bool lighting_history_compatible(
    const LightingTemporalSample& current, const LightingTemporalSample& history,
    const LightingTemporalSettings& settings) noexcept;
[[nodiscard]] LightingTemporalSample accumulate_lighting_history(
    const LightingTemporalSample& current, const LightingTemporalSample& history,
    const LightingTemporalSettings& settings, bool resetHistory = false) noexcept;

// Penumbra width at a receiver (metres): 2 tan(sun angular radius) x the mean distance to the
// occluders the soft shadow rays hit (resolve_shadows.hlsl). 0 when nothing was hit.
[[nodiscard]] float shadow_penumbra_width_meters(float sunAngularRadius, float meanBlockerDistanceMeters) noexcept;

// The shadow channel of spatial_lighting_denoise.hlsl on a CPU image (row-major, width x height):
// a cross-bilateral Gaussian whose sigma is a quarter of the widest penumbra under the filter, in
// pixels at the centre's depth, clamped to [0.25, spatialSigma]. radiansPerPixel <= 0 keeps the
// fixed sigma (the old filter).
struct ShadowDenoiseSettings {
    std::uint32_t radius{2};
    float spatialSigma{1.5F};
    float depthSigmaMeters{0.1F};
    float normalPower{8.0F};
    float radiansPerPixel{0.0F};
};
// gDenoiseRadiansPerPixel for a perspective view: the angle one pixel covers at the centre.
[[nodiscard]] float denoise_radians_per_pixel(float verticalFovRadians, std::uint32_t heightPixels) noexcept;
[[nodiscard]] std::vector<float> denoise_shadow_visibility(
    std::uint32_t width, std::uint32_t height, std::span<const float> visibility,
    std::span<const float> penumbraMeters, std::span<const float> depthMeters,
    std::span<const Float3> normals, const ShadowDenoiseSettings& settings);

} // namespace dve::render
