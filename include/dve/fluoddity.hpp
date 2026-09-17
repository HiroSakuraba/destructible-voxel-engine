#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dve {

enum class FluoddityParameter : std::uint8_t {
    SensorGain,
    SensorAngle,
    SensorDistance,
    MutationScale,
    GlobalForceMultiplier,
    Drag,
    AxialForce,
    LateralForce,
    StrafePower,
    TrailPersistence,
    TrailDiffusion,
    HazardRate,
    Count,
};

inline constexpr std::size_t kFluoddityParameterCount =
    static_cast<std::size_t>(FluoddityParameter::Count);
inline constexpr std::size_t kFluoddityFourierCenterCount = 10U;
inline constexpr std::size_t kFluoddityRuleFloatCount = 120U;
inline constexpr std::uint32_t kFluoddityMaximumCohorts = 16'384U;

enum class FluoddityBoundaryMode : std::uint8_t { Bounce = 0, Reset = 1, Wrap = 2 };
enum class FluoddityInitialCondition : std::uint8_t {
    FlatGrid = 0,
    RandomSphere = 1,
    SphericalShell = 2,
    Grid3D = 3,
};
enum class FluodditySeedAlgorithm : std::uint8_t {
    ImportedFloatPcg = 0,
    NativeIntegerPcg = 1,
};
enum class FluoddityTrailMode : std::uint8_t {
    VelocityRgb = 0,
    VelocityRgbDensityA = 1,
};
enum class FluoddityQuality : std::uint8_t { Low, Medium, High, Custom };
enum class FluoddityAccumulationMode : std::uint8_t {
    FixedPointSigned = 0,
    NativeFloatAtomic = 1,
};

struct FluoddityParameterSetting {
    float value{};
    float minimum{};
    float maximum{};
    float defaultMinimum{};
    float defaultMaximum{};
    float xSweep{};
    float ySweep{};
    float cohortSweep{};
    float jitter{};
};

struct FluoddityFourierCenter {
    std::array<float, 4> frequency{};
    std::array<float, 4> amplitude{};
    std::array<float, 2> frequencyExtension{};
    std::array<float, 2> amplitudeExtension{};
};

using FluoddityRule = std::array<FluoddityFourierCenter, kFluoddityFourierCenterCount>;

struct FluoddityCompatibilityMetadata {
    bool hadFieldStrengths{};
    std::optional<float> forceFieldStrength;
    std::optional<float> strafeFieldStrength;
    std::optional<std::int32_t> absoluteOrientation;
    std::optional<float> orientationMix;
    std::optional<float> inkWeight;
    std::optional<bool> watercolorMode;
    std::optional<std::int32_t> planeSamples;
    std::optional<bool> testingMode;
};

struct FluoddityRuleAsset {
    std::string name{"Fluoddity Rule"};
    std::string sourcePreset;
    std::uint32_t sourceVersion{};
    std::array<FluoddityParameterSetting, kFluoddityParameterCount> parameters{};
    bool parameterSweepsEnabled{};
    bool disableSymmetry{};
    float gravityForce{};
    float gravityStrafe{};
    FluoddityBoundaryMode boundaryMode{FluoddityBoundaryMode::Bounce};
    FluoddityInitialCondition initialCondition{FluoddityInitialCondition::FlatGrid};
    float initialSpacing{1.0F};
    std::uint32_t cohortCount{64U};
    float importedRuleSeed{};
    std::uint64_t nativeSeed{};
    FluodditySeedAlgorithm seedAlgorithm{FluodditySeedAlgorithm::ImportedFloatPcg};
    float hueSensitivity{0.5F};
    bool colorByCohort{true};
    std::uint32_t sourceCanvasResolution{256U};
    FluoddityTrailMode trailMode{FluoddityTrailMode::VelocityRgb};
    FluoddityRule rule{};
    std::string notes;
    FluoddityCompatibilityMetadata compatibility{};
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash();
};

struct FluoddityImportResult {
    std::optional<FluoddityRuleAsset> asset;
    std::vector<std::string> warnings;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return asset.has_value(); }
};

struct FluoddityPresetBatchResult {
    std::vector<FluoddityRuleAsset> assets;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

[[nodiscard]] FluoddityImportResult import_fluoddity_preset_json(
    const std::filesystem::path& path);
[[nodiscard]] FluoddityPresetBatchResult import_fluoddity_preset_directory(
    const std::filesystem::path& directory);
[[nodiscard]] bool write_dfluoddity(
    const std::filesystem::path& path,
    const FluoddityRuleAsset& asset,
    std::string* error = nullptr);
[[nodiscard]] FluoddityImportResult read_dfluoddity(const std::filesystem::path& path);

[[nodiscard]] std::array<float, 6> evaluate_fluoddity_rule(
    const FluoddityRule& rule,
    const std::array<float, 6>& input) noexcept;
[[nodiscard]] FluoddityRule generate_fluoddity_rule_from_imported_seed(float seed) noexcept;
[[nodiscard]] FluoddityRule mutate_fluoddity_rule(
    const FluoddityRule& rule,
    float amount,
    float cohort) noexcept;
[[nodiscard]] float evaluate_fluoddity_parameter(
    const FluoddityParameterSetting& setting,
    float positionX,
    float positionY,
    float cohort,
    std::uint32_t cohortCount,
    std::uint32_t frameNumber = 0U) noexcept;

struct FluoddityQualityProfile {
    FluoddityQuality quality{FluoddityQuality::Medium};
    std::uint32_t particleCapacity{1'000'000U};
    std::uint32_t trailResolution{192U};
    FluoddityAccumulationMode accumulationMode{FluoddityAccumulationMode::FixedPointSigned};
};

struct FluoddityMemoryEstimate {
    std::uint64_t particleBytes{};
    std::uint64_t trailBytes{};
    std::uint64_t accumulationBytes{};
    std::uint64_t ruleBytes{};
    std::uint64_t supportingBytes{};
    std::uint64_t totalBytes{};
};

[[nodiscard]] FluoddityQualityProfile fluoddity_quality_profile(FluoddityQuality quality) noexcept;
[[nodiscard]] FluoddityMemoryEstimate estimate_fluoddity_memory(
    const FluoddityQualityProfile& profile,
    std::uint32_t cohortCount = 64U) noexcept;
[[nodiscard]] std::string fluoddity_parameter_name(FluoddityParameter parameter);
[[nodiscard]] std::string fluoddity_quality_name(FluoddityQuality quality);

} // namespace dve
