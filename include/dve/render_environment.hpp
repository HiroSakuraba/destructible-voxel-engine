#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "dve/render/ies_profile.hpp"
#include "dve/types.hpp"

namespace dve {

// World scale for metre-authored lighting distances. Every `*Meters` field of RenderEnvironment
// is in metres on every backend: the GPU divides by GpuRenderEnvironment::metersPerVoxel
// (MetersToVoxelUnits in render_environment.hlsli) and the CPU reference renderer divides by
// ReferenceVoxelRenderer::metersPerVoxel, because both trace in voxel-index units. This default
// matches the engine's authored voxel size (GameObjectDesc/VoxelizeSettings/EditorDocument
// voxelSizeMeters = 0.1 m); callers with a different uniform voxel size pass their own
// (e.g. RuntimeScene::uniform_voxel_size_meters()).
inline constexpr float kDefaultMetersPerVoxel = 0.10F;
inline constexpr std::uint32_t kMaximumAmbientOcclusionSamples = 64U;

// Returns metersPerVoxel when finite and positive, otherwise kDefaultMetersPerVoxel.
[[nodiscard]] constexpr float resolve_meters_per_voxel(float metersPerVoxel) noexcept {
    return (metersPerVoxel > 0.0F && metersPerVoxel <= 3.0e38F) ? metersPerVoxel
                                                                  : kDefaultMetersPerVoxel;
}

enum class TonemapOperator : std::uint32_t { ACES = 0, Reinhard = 1, Clamp = 2, AgX = 3, PBRNeutral = 4 };

// Indirect-lighting policy. AmbientHemisphere is a cheap sky/ground fallback; VoxelOneBounce
// traces a bounded diffuse bounce through the authoritative voxel scene. New projects use the
// one-bounce mode so global illumination is genuinely enabled rather than represented by a UI
// checkbox that leaves the renderer unchanged. RadianceCascades is the CPU-reference screen-probe
// radiance-cascades solve (same one-bounce radiance model, no sample cap); GPU backends do not
// implement it yet and fall back to VoxelOneBounce (see pack_gpu_render_environment).
enum class GlobalIlluminationMode : std::uint32_t {
    Off = 0,
    AmbientHemisphere = 1,
    VoxelOneBounce = 2,
    RadianceCascades = 3,
};

// Directional-shadow policy used by the shared environment. Hard is one exact sun ray, Soft
// samples a finite angular sun disk, Contact limits a high-detail ray to nearby blockers, and
// Hybrid combines soft directional visibility with a short contact ray.
enum class ShadowMode : std::uint32_t {
    Off = 0,
    Hard = 1,
    Soft = 2,
    Contact = 3,
    Hybrid = 4,
};

// Where the sky light comes from. Colors: the hand-set skyColor / groundColor blend and a sun of
// fixed sunColor (the original behaviour). Physical: an Earth-like atmosphere (Rayleigh, Mie and
// ozone; render/atmosphere.hpp) lit by the sun drives the background sky, the sun's colour at the
// ground (orange at sunset, gone below the horizon), the sky light on surfaces and in their
// reflections, and aerial perspective; skyColor / groundColor are then unused.
enum class SkyMode : std::uint32_t {
    Colors = 0,
    Physical = 1,
};

// Positions and emitter edges are authored in metres; intensity is RGB radiant
// intensity for point/spot/candle, emitted radiance for area lights. Direction
// points out of the light.
inline constexpr std::size_t kMaximumSceneLights = 4096U;
enum class SceneLightKind : std::uint32_t { Point, Spot, Quad, Disc, Candle };
struct SceneLight {
  SceneLightKind kind{SceneLightKind::Point};
  Float3 positionMeters{};
  Float3 direction{0, -1, 0};
  Float3 edgeU{1, 0, 0}, edgeV{0, 0, 1}; // full quad edges; disc radius vectors
  Float3 color{1, 1, 1};
  float intensity{1};
  float innerConeRadians{0.35F}, outerConeRadians{0.6F};
  bool twoSided{};
  float flickerAmount{0.15F}, flickerFrequencyHz{8};
  std::uint32_t seed{1};
  // Optional IES photometric profile (CPU lighting only; GPU packing ignores
  // it). Reshapes a Point/Spot/Candle light's angular distribution.
  std::shared_ptr<const render::IesProfile> iesProfile;
  [[nodiscard]] bool validate(std::string *error = nullptr) const noexcept;
};

// The Godot-"Environment"-resource-shaped settings object: lighting, global illumination,
// shadows, exposure, tonemap, and bloom live in one validated place. CPU reference rendering,
// GPU constant packing, scripts, and editor settings all consume the same defaults. Distances
// named `*Meters` are metres on the CPU and GPU alike (see kDefaultMetersPerVoxel); the defaults
// below were chosen as metres for the GPU path.
struct RenderEnvironment {
    std::vector<SceneLight> lights; // maximum 4096; empty preserves legacy scenes
    std::uint32_t localLightSamples{4}; // power-weighted samples per hit, 1..64
    // Zero preserves legacy GI; 1..64 enables the hybrid diffuse/glossy path solver.
    std::uint32_t pathTraceBounces{0};
    std::uint32_t pathTraceSamples{4}; // 1..256
    bool pathTraceRoulette{true};
    double lightingTimeSeconds{}; // simulation time; no wall-clock randomness

    Float3 sunDirection{0.45F, 0.80F, 0.35F}; // toward the sun; normalized when packed
    float sunIntensity{2.4F};
    Float3 sunColor{1.0F, 0.98F, 0.95F};
    float subsurfaceMaxDistanceMeters{0.5F};
    Float3 skyColor{0.22F, 0.24F, 0.28F};
    float exposure{1.0F};
    Float3 groundColor{0.16F, 0.17F, 0.19F};
    TonemapOperator tonemapOperator{TonemapOperator::ACES};
    Float3 globalTint{1.0F, 1.0F, 1.0F};
    float bloomThreshold{1.0F};
    float bloomIntensity{0.08F};
    float bloomRadius{4.0F};

    // GI is on by default. Four bounded rays provide visible color transfer in the reference
    // path while remaining cheap enough for editor previews; production backends may map the
    // same contract onto temporal accumulation or a probe/cache implementation.
    GlobalIlluminationMode globalIlluminationMode{GlobalIlluminationMode::VoxelOneBounce};
    float globalIlluminationIntensity{0.65F};
    float globalIlluminationMaxDistanceMeters{12.0F};
    std::uint32_t globalIlluminationSamples{4};

    // Soft directional shadows are the default presentation. Contact and Hybrid are available
    // for close-up work without forcing the most expensive mode on every project.
    ShadowMode shadowMode{ShadowMode::Soft};
    float shadowStrength{0.85F};
    float shadowSoftnessRadians{0.012F};
    std::uint32_t shadowSamples{4};
    float shadowMaxDistanceMeters{2500.0F};
    float contactShadowDistanceMeters{2.0F};
    float shadowBiasMeters{0.015F};

    // Ambient occlusion: how much of the sky a surface point sees, from cosine-weighted rays out
    // to ambientOcclusionMaxDistanceMeters. A ray that hits at distance d occludes
    // (1 - d / max)^2: near geometry counts fully and farther geometry fades out, so open ground
    // beside a wall is not darkened by a cliff a mile away. It scales the ambient light wherever
    // nothing traced already accounts for occlusion: the AmbientHemisphere ambient on voxels and
    // the polygon ambient (traced GI modes see occluders through their own rays). 0 samples =
    // off. The GPU passes (generate_ao_rays, resolve_ao) use the same rays and weighting.
    std::uint32_t ambientOcclusionSamples{8};
    float ambientOcclusionMaxDistanceMeters{1.0F};

    // Distance fog, from the frame's depth (render::apply_distance_fog): a surface d metres away
    // along the view ray keeps exp(-density x max(0, d - start)) of its light and the rest becomes
    // fogColor (radiance, like skyColor). 0 density = off.
    float fogDensityPerMeter{0.0F};
    float fogStartMeters{0.0F};
    Float3 fogColor{0.62F, 0.68F, 0.76F};
    // Height falloff for the distance fog: above fogBaseHeightMeters the density scales by
    // exp(-fogHeightFalloffPerMeter x (height - base)), so fog thins with altitude (valley fog
    // under clear hilltops). At and below the base height the density is fogDensityPerMeter.
    // 0 falloff (the default) = uniform density at every height, the pre-falloff behaviour.
    float fogHeightFalloffPerMeter{0.0F};
    float fogBaseHeightMeters{0.0F};

    // Physical sky (skyMode Physical). sunColor x sunIntensity is then the sunlight arriving at the
    // top of the atmosphere; the air in between colours and dims it. The scales multiply the
    // Earth densities (Bruneton / Hillaire values): Rayleigh air (blue sky, red sunsets), Mie
    // aerosols (haze, the bright halo around the sun; 1 = a clear day, 5-20 = hazy) and ozone
    // (the deep blue of twilight). mieAnisotropy is the Cornette-Shanks g (forward scattering).
    // World y = 0 sits at observerAltitudeMeters. Real aerial perspective is faint over the few
    // hundred metres of a game scene; aerialPerspectiveScale multiplies the distances it sees
    // (1 = physical). sunDisc scales the drawn solar disc (0 = none).
    SkyMode skyMode{SkyMode::Colors};
    float atmosphereRayleighScale{1.0F};
    float atmosphereMieScale{1.0F};
    float atmosphereOzoneScale{1.0F};
    float atmosphereMieAnisotropy{0.8F};
    float atmosphereGroundAlbedo{0.3F};
    float observerAltitudeMeters{0.0F};
    float aerialPerspectiveScale{1.0F};
    float sunDisc{1.0F};

    // Volumetric fog (render/volumetric_fog.hpp): a height fog that scatters sunlight, ray-marched
    // per pixel with a shadow ray toward the sun at every sample, so shadowed air stays dark and
    // lit air glows: light shafts. Extinction is volumetricFogDensityPerMeter up to
    // volumetricFogBaseHeightMeters (world y in metres) and falls off as
    // exp(-falloff x height above it) (0 = uniform). Albedo is the scattered share (colour);
    // anisotropy the Henyey-Greenstein g (forward scattering: bright toward the sun). The march
    // covers volumetricFogMaxDistanceMeters from the camera in volumetricFogSamples steps.
    // 0 density = off.
    // Traced displacement (VoxelMaterialDefinition::displacementDepthMeters) reshapes voxels within
    // this distance of the camera; beyond it the same materials fall back to parallax shading.
    float displacementTraceDistanceMeters{100.0F};

    float volumetricFogDensityPerMeter{0.0F};
    float volumetricFogHeightFalloffPerMeter{0.05F};
    float volumetricFogBaseHeightMeters{0.0F};
    Float3 volumetricFogAlbedo{0.9F, 0.9F, 0.9F};
    float volumetricFogAnisotropy{0.6F};
    float volumetricFogMaxDistanceMeters{100.0F};
    std::uint32_t volumetricFogSamples{32};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct Component;
struct RigidTransform;
inline constexpr const char *kSceneLightComponent = "dve.light";
[[nodiscard]] std::optional<SceneLight>
scene_light_from_component(const Component &component,
                           const RigidTransform &worldTransform,
                           std::string *error = nullptr);
// Portable, versioned lighting extension for scenes/settings and script saves.
// Decode is atomic and leaves unrelated environment fields alone. Classic
// locale, round-trip precision.
[[nodiscard]] std::string
encode_scene_lighting(const RenderEnvironment &environment);
[[nodiscard]] bool decode_scene_lighting(const std::string &text,
                                         RenderEnvironment &environment,
                                         std::string *error = nullptr);
} // namespace dve
