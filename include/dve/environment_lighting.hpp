#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/camera_system.hpp"

namespace dve {

enum class CubeFace : std::uint8_t { PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ, NegativeZ };


struct EnvironmentImage2D {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Float3> pixels;
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct EnvironmentCube {
    std::uint32_t resolution{};
    std::vector<Float3> texels; // six tightly packed faces in CubeFace order
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
    [[nodiscard]] std::size_t face_offset(CubeFace face) const noexcept;
};

struct EnvironmentCubeMipChain {
    std::vector<EnvironmentCube> levels;
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct CubeLookup {
    CubeFace face{CubeFace::PositiveX};
    Float2 uv{0.5F, 0.5F};
};

[[nodiscard]] CubeLookup direction_to_cube_lookup(Float3 direction) noexcept;
[[nodiscard]] Float3 cube_lookup_to_direction(CubeFace face, Float2 uv) noexcept;
[[nodiscard]] Float3 sample_environment_image(const EnvironmentImage2D& image, Float2 uv) noexcept;
[[nodiscard]] Float3 sample_environment_cube(const EnvironmentCube& cube, Float3 direction) noexcept;
[[nodiscard]] Float3 sample_environment_cube_lod(const EnvironmentCubeMipChain& chain,
                                                  Float3 direction, float lod) noexcept;
// Build a seamless, filtered source mip chain for CPU-side cubemap sampling and GGX prefiltering.
[[nodiscard]] EnvironmentCubeMipChain build_environment_cube_mip_chain(
    const EnvironmentCube& source);
[[nodiscard]] EnvironmentCube convert_equirectangular_to_cube(const EnvironmentImage2D& image,
                                                               std::uint32_t faceResolution);

struct IblBakeSettings {
    std::uint32_t irradianceResolution{8};
    std::uint32_t specularResolution{16};
    std::uint32_t specularMipLevels{5};
    std::uint32_t sampleCount{64};
    std::uint32_t brdfResolution{32};
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct IblBrdfSample {
    float scale{};
    float bias{};
};

[[nodiscard]] IblBrdfSample integrate_environment_brdf(float ndotv, float roughness,
                                                        std::uint32_t sampleCount) noexcept;

struct IblBakeResult {
    EnvironmentCube diffuseIrradiance;
    EnvironmentCubeMipChain specularPrefilter;
    std::uint32_t brdfResolution{};
    std::vector<IblBrdfSample> brdfLut;
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] IblBakeResult bake_image_based_lighting(const EnvironmentCube& source,
                                                       const IblBakeSettings& settings = {});

struct IblMaterialInput {
    Float3 normal{0.0F, 1.0F, 0.0F};
    Float3 viewDirection{0.0F, 0.0F, 1.0F};
    Float3 baseColor{1.0F, 1.0F, 1.0F};
    float metallic{};
    float roughness{0.5F};
    float specular{0.5F};
    float ambientOcclusion{1.0F};
};

[[nodiscard]] Float3 evaluate_image_based_lighting(const IblBakeResult& ibl,
                                                    const IblMaterialInput& material) noexcept;

using ReflectionProbeId = std::uint64_t;
struct ReflectionProbe {
    ReflectionProbeId id{};
    Float3 position{};
    Float3 halfExtents{5.0F, 5.0F, 5.0F};
    float blendDistance{1.0F};
    float intensity{1.0F};
    std::int32_t priority{};
    bool boxProjection{true};
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct ReflectionProbeWeight {
    ReflectionProbeId id{};
    float weight{};
};

[[nodiscard]] std::vector<ReflectionProbeWeight> select_reflection_probes(
    const std::vector<ReflectionProbe>& probes, Float3 worldPosition,
    std::uint32_t maximumProbeCount = 4U);
[[nodiscard]] Float3 box_project_reflection_direction(const ReflectionProbe& probe,
                                                       Float3 worldPosition,
                                                       Float3 reflectionDirection) noexcept;


struct EnvironmentLightingAsset {
    std::string name{"Environment Lighting"};
    float rotationRadians{};
    float intensity{1.0F};
    float skyboxExposure{};
    EnvironmentCube sourceRadiance;
    IblBakeResult baked;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
    void recompute_content_hash() noexcept;
};

struct EnvironmentLightingReadResult {
    std::optional<EnvironmentLightingAsset> asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return asset.has_value(); }
};

[[nodiscard]] EnvironmentLightingAsset make_default_environment_lighting_asset(
    const IblBakeSettings& settings = {});
[[nodiscard]] bool write_dveibl(const std::filesystem::path& path,
                                const EnvironmentLightingAsset& asset,
                                std::string* error = nullptr);
[[nodiscard]] EnvironmentLightingReadResult read_dveibl(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 1024ULL * 1024ULL * 1024ULL);

struct EnvironmentSkyboxSettings {
    std::uint32_t width{320U};
    std::uint32_t height{180U};
    float rotationRadians{};
    float intensity{1.0F};
    float exposure{};
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] Float3 rotate_environment_direction(Float3 direction,
                                                   float yawRadians) noexcept;
[[nodiscard]] Float3 sample_environment_sky(const EnvironmentCube& cube,
                                            Float3 direction,
                                            float rotationRadians,
                                            float intensity,
                                            float exposure) noexcept;
[[nodiscard]] std::vector<Float4> render_environment_skybox(
    const EnvironmentCube& cube,
    const camera::CameraPose& camera,
    const EnvironmentSkyboxSettings& settings = {});

} // namespace dve
