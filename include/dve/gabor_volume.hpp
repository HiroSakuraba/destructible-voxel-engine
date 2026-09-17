#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/transform.hpp"
#include "dve/types.hpp"

namespace dve {

enum class GaborVolumeMode : std::uint8_t {
    AbsorptionPreview,
    EmissionAbsorption,
    ScatteringExperimental,
};

enum class GaborVolumeQuality : std::uint8_t { Low, Medium, High, Cinematic };
enum class GaborVolumeDebugView : std::uint8_t {
    Disabled,
    PrimitiveBounds,
    ActiveLod,
    FrequencyBands,
    OrientationBins,
    TileOccupancy,
    RaySteps,
};

struct GaborVolumePrimitive {
    Float3 center{};
    Float3 scale{1.0F, 1.0F, 1.0F};
    Quaternion rotation{};
    Float3 albedo{1.0F, 1.0F, 1.0F};
    float opacity{1.0F};
    float frequency{};
    float extent{3.0F};
    std::uint16_t lodLevel{};
    std::uint16_t orientationBin{};
};

struct GaborVolumeMaterial {
    float densityMultiplier{1.0F};
    Float3 albedoTint{1.0F, 1.0F, 1.0F};
    Float3 emissionColor{};
    float emissionIntensity{};
    float anisotropy{};
    float shadowStrength{0.75F};
    float maximumRayDistance{64.0F};
    float lodBias{};
    float temporalAccumulation{0.85F};
};

struct GaborVolumeAsset {
    std::string name{"Gabor Volume"};
    Float3 boundsMinimum{};
    Float3 boundsMaximum{};
    GaborVolumeMaterial material{};
    std::vector<GaborVolumePrimitive> primitives;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_bounds_and_hash();
};

struct GaborPlyImportOptions {
    std::size_t maximumPrimitives{1'000'000U};
    std::uint16_t forcedLodLevel{};
    bool decodeLogScales{true};
    bool normalizeQuaternion{true};
    float positionScale{1.0F};
    float opacityScale{1.0F};
    float densityNormalization{1.0F};
};

struct GaborImportResult {
    std::optional<GaborVolumeAsset> asset;
    std::vector<std::string> warnings;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return asset.has_value(); }
};

[[nodiscard]] GaborImportResult import_gabor_ply(
    const std::filesystem::path& path,
    const GaborPlyImportOptions& options = {});

[[nodiscard]] GaborImportResult import_gabor_pyramid(
    const std::filesystem::path& directory,
    const GaborPlyImportOptions& options = {});

[[nodiscard]] bool write_dgabor(
    const std::filesystem::path& path,
    const GaborVolumeAsset& asset,
    std::string* error = nullptr);

[[nodiscard]] GaborImportResult read_dgabor(const std::filesystem::path& path);

struct GaborVolumeRenderSettings {
    bool enabled{true};
    GaborVolumeMode mode{GaborVolumeMode::EmissionAbsorption};
    GaborVolumeQuality quality{GaborVolumeQuality::Medium};
    bool continuousLod{true};
    bool temporalAccumulation{true};
    bool castVolumeShadows{true};
    bool receiveSceneShadows{true};
    GaborVolumeDebugView debugView{GaborVolumeDebugView::Disabled};
    std::uint32_t maximumPrimitivesPerTile{512U};
    float lodBias{};
};

struct GaborVolumeFramePlan {
    bool enabled{};
    GaborVolumeMode mode{GaborVolumeMode::EmissionAbsorption};
    GaborVolumeQuality quality{GaborVolumeQuality::Medium};
    std::uint32_t raySteps{};
    std::size_t submittedPrimitives{};
    std::size_t culledPrimitives{};
    bool temporalHistory{};
    bool shadowWork{};
};

[[nodiscard]] float gabor_lod_weight(
    const GaborVolumePrimitive& primitive,
    float projectedPixels,
    float lodBias = 0.0F) noexcept;

[[nodiscard]] GaborVolumeFramePlan plan_gabor_volume_frame(
    const GaborVolumeAsset& asset,
    const GaborVolumeRenderSettings& settings,
    float projectedDiameterPixels = 256.0F) noexcept;

[[nodiscard]] float evaluate_gabor_density(
    const GaborVolumeAsset& asset,
    Float3 worldPosition,
    float projectedPixels = 256.0F,
    bool continuousLod = true,
    float lodBias = 0.0F) noexcept;

struct GaborPreviewOptions {
    std::uint32_t width{320U};
    std::uint32_t height{240U};
    std::uint32_t raySteps{96U};
    Float3 background{0.05F, 0.06F, 0.08F};
};

[[nodiscard]] bool render_gabor_preview_ppm(
    const std::filesystem::path& path,
    const GaborVolumeAsset& asset,
    const GaborPreviewOptions& options = {},
    std::string* error = nullptr);

[[nodiscard]] std::string gabor_volume_mode_name(GaborVolumeMode mode);
[[nodiscard]] std::string gabor_volume_quality_name(GaborVolumeQuality quality);

} // namespace dve
