#pragma once

#include <cstdint>
#include <string>

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

enum class TonemapOperator : std::uint32_t { ACES = 0, Reinhard = 1, Clamp = 2 };

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

// The Godot-"Environment"-resource-shaped settings object: lighting, global illumination,
// shadows, exposure, tonemap, and bloom live in one validated place. CPU reference rendering,
// GPU constant packing, scripts, and editor settings all consume the same defaults. Distances
// named `*Meters` are metres on the CPU and GPU alike (see kDefaultMetersPerVoxel); the defaults
// below were chosen as metres for the GPU path.
struct RenderEnvironment {
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

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

} // namespace dve
