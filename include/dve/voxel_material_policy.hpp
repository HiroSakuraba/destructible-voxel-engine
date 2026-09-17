#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve {

enum class VoxelMaterialMode : std::uint8_t {
    BakedProperties,
    SingleMaterial,
    DeferredPalette,
    Hybrid,
};

enum class VoxelMaterialPlatformProfile : std::uint8_t {
    Editor,
    HighEndDesktop,
    Console,
    IntegratedGpu,
    Mobile,
    DedicatedServer,
};

enum class VoxelMaterialPaletteOverflowPolicy : std::uint8_t {
    DominantMaterial,
    BakeProperties,
    RequestRecook,
    Reject,
};

enum class VoxelMaterialRuntimePath : std::uint8_t {
    NoPath,
    BakedProperties,
    SingleMaterial,
    DeferredPalette2,
    DeferredPalette4,
};

struct VoxelMaterialCookedRepresentations {
    bool bakedProperties{};
    bool singleMaterial{};
    bool deferredPalette2{};
    bool deferredPalette4{};
};

struct VoxelMaterialPolicyConfig {
    VoxelMaterialMode mode{VoxelMaterialMode::Hybrid};
    VoxelMaterialPlatformProfile platform{VoxelMaterialPlatformProfile::HighEndDesktop};
    VoxelMaterialPaletteOverflowPolicy overflowPolicy{VoxelMaterialPaletteOverflowPolicy::BakeProperties};
    float deferredMaximumDistance{40.0F};
    std::uint32_t deferredMaximumLod{1U};
    std::uint8_t maximumPaletteSlots{4U};
    bool retainBakedFallback{true};
    bool retainSingleMaterialFallback{true};
    bool allowRuntimeSwitching{true};
    bool enableFourWayBlending{true};
    bool preferDeferredForDestructible{true};
    bool fallBackUnderMemoryPressure{true};
};

struct VoxelMaterialAssetOverride {
    std::optional<VoxelMaterialMode> mode;
    std::optional<float> deferredMaximumDistance;
    std::optional<std::uint32_t> deferredMaximumLod;
    std::optional<std::uint8_t> maximumPaletteSlots;
    std::optional<VoxelMaterialPaletteOverflowPolicy> overflowPolicy;
    std::optional<bool> retainBakedFallback;
    std::optional<bool> retainSingleMaterialFallback;
    std::optional<bool> allowRuntimeSwitching;
    std::optional<bool> enableFourWayBlending;
};

struct VoxelMaterialBrickContext {
    std::uint64_t brickId{};
    std::string assetName;
    float cameraDistance{};
    std::uint32_t lod{};
    std::uint8_t sourceMaterialCount{1U};
    float dominantMaterialWeight{1.0F};
    bool destructible{};
    bool recentlyModified{};
    bool memoryPressure{};
    bool canonicalMaterialMembershipAvailable{true};
    VoxelMaterialCookedRepresentations cooked;
};

struct VoxelMaterialSelectionRequest {
    VoxelMaterialPolicyConfig project;
    VoxelMaterialAssetOverride asset;
    VoxelMaterialBrickContext brick;
    std::optional<VoxelMaterialMode> editorPreviewMode;
};

struct VoxelMaterialSelectionDecision {
    std::uint64_t brickId{};
    std::string assetName;
    VoxelMaterialMode requestedMode{VoxelMaterialMode::Hybrid};
    VoxelMaterialMode effectiveMode{VoxelMaterialMode::Hybrid};
    VoxelMaterialRuntimePath runtimePath{VoxelMaterialRuntimePath::NoPath};
    std::uint8_t materialSlotsUsed{};
    std::uint32_t estimatedTextureSamples{};
    std::uint32_t additionalTextureSamples{};
    bool usedFallback{};
    bool recookRequested{};
    bool runtimeSwitchAvailable{};
    bool sourceDataSufficient{};
    bool valid{};
    std::string reason;
    std::uint64_t contentHash{};
};

struct VoxelMaterialPolicyReport {
    std::uint64_t brickCount{};
    std::uint64_t bakedBrickCount{};
    std::uint64_t singleMaterialBrickCount{};
    std::uint64_t deferredPalette2BrickCount{};
    std::uint64_t deferredPalette4BrickCount{};
    std::uint64_t fallbackBrickCount{};
    std::uint64_t recookRequestCount{};
    std::uint64_t invalidBrickCount{};
    std::uint64_t estimatedTextureSamples{};
    std::uint64_t additionalTextureSamples{};
    std::vector<VoxelMaterialSelectionDecision> decisions;
    std::uint64_t contentHash{};
};

[[nodiscard]] VoxelMaterialPolicyConfig resolve_voxel_material_policy(
    const VoxelMaterialPolicyConfig& project,
    const VoxelMaterialAssetOverride& asset) noexcept;
[[nodiscard]] VoxelMaterialSelectionDecision select_voxel_material_path(
    const VoxelMaterialSelectionRequest& request);
[[nodiscard]] VoxelMaterialPolicyReport build_voxel_material_policy_report(
    std::span<const VoxelMaterialSelectionRequest> requests);
[[nodiscard]] std::string voxel_material_policy_json(const VoxelMaterialPolicyReport& report);

[[nodiscard]] const char* to_string(VoxelMaterialMode value) noexcept;
[[nodiscard]] const char* to_string(VoxelMaterialPlatformProfile value) noexcept;
[[nodiscard]] const char* to_string(VoxelMaterialPaletteOverflowPolicy value) noexcept;
[[nodiscard]] const char* to_string(VoxelMaterialRuntimePath value) noexcept;

} // namespace dve
