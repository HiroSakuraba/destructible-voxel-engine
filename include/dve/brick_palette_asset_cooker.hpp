#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/voxel_material_switcher.hpp"

namespace dve {

struct BrickPaletteRetentionProfile {
    bool retainBakedProperties{true};
    bool retainSingleMaterial{true};
    bool retainPalette2{true};
    bool retainPalette4{true};
    bool retainCanonicalSource{true};
};

struct BrickPaletteAssetCookSettings {
    BrickPaletteRetentionProfile retention{};
    VoxelMaterialPaletteOverflowPolicy palette2Overflow{
        VoxelMaterialPaletteOverflowPolicy::BakeProperties};
    VoxelMaterialPaletteOverflowPolicy palette4Overflow{
        VoxelMaterialPaletteOverflowPolicy::BakeProperties};
    render::BrickPaletteShadingConfig bakedShading{};
    float voxelSizeMeters{0.10F};
};

struct CookedBrickMaterialRepresentations {
    BrickKey key{};
    std::uint32_t sourceGeneration{};
    std::uint64_t dependencyKey{};
    VoxelMaterialRepresentationSet representations;
};

struct CookedBrickPaletteAsset {
    std::uint64_t objectId{};
    std::uint64_t dependencyKey{};
    std::uint64_t generation{};
    BrickPaletteRetentionProfile retention{};
    BrickPaletteRemapTable remap;
    std::vector<CookedBrickMaterialRepresentations> bricks;
    std::uint64_t retainedBytes{};
    std::uint64_t canonicalBytes{};
    std::uint64_t contentHash{};
};

struct BrickPaletteAssetRecookResult {
    bool valid{};
    std::uint64_t submittedEdits{};
    std::uint64_t uniqueBrickEdits{};
    std::uint64_t recookedBricks{};
    std::uint64_t removedBricks{};
    std::uint64_t unchangedBricks{};
    std::uint64_t generation{};
    std::string reason;
};


struct CookedVoxelAssetPalettePackage {
    CookedVoxelAsset asset;
    CookedBrickPaletteAsset brickPalettes;
    bool success{};
};

[[nodiscard]] CookedVoxelAssetPalettePackage package_cooked_voxel_asset_brick_palettes(
    CookedVoxelAsset asset,
    const BrickPaletteAssetCookSettings& settings = {});

[[nodiscard]] CookedVoxelAssetPalettePackage voxelize_scene_with_brick_palettes(
    const ImportedScene& scene,
    const VoxelizeSettings& voxelSettings = {},
    const BrickPaletteAssetCookSettings& paletteSettings = {});

[[nodiscard]] CookedVoxelAssetPalettePackage cook_model_with_brick_palettes(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions = {},
    const VoxelizeSettings& voxelSettings = {},
    const BrickPaletteAssetCookSettings& paletteSettings = {});

[[nodiscard]] std::vector<render::BrickPaletteMaterialRecord>
make_brick_palette_material_records(std::span<const VoxelMaterialDefinition> materials);

[[nodiscard]] CookedBrickPaletteAsset cook_voxel_object_brick_palettes(
    const VoxelObject& object,
    std::span<const VoxelMaterialDefinition> materials,
    const BrickPaletteAssetCookSettings& settings = {});

[[nodiscard]] BrickPaletteAssetRecookResult recook_voxel_object_brick_palettes(
    CookedBrickPaletteAsset& asset,
    const VoxelObject& object,
    std::span<const VoxelMaterialDefinition> materials,
    std::span<const AppliedBrickEdit> edits,
    const BrickPaletteAssetCookSettings& settings = {});

[[nodiscard]] const CookedBrickMaterialRepresentations* find_cooked_brick_materials(
    const CookedBrickPaletteAsset& asset, BrickKey key) noexcept;

[[nodiscard]] bool validate_cooked_brick_palette_asset(
    const CookedBrickPaletteAsset& asset, std::string* error = nullptr);

[[nodiscard]] std::uint64_t brick_palette_asset_dependency_key(
    const VoxelObject& object,
    std::span<const VoxelMaterialDefinition> materials,
    const BrickPaletteAssetCookSettings& settings) noexcept;

} // namespace dve
