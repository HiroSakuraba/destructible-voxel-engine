#include "dve/brick_palette_asset_cooker.hpp"

namespace dve {

CookedVoxelAssetPalettePackage voxelize_scene_with_brick_palettes(
    const ImportedScene& scene,
    const VoxelizeSettings& voxelSettings,
    const BrickPaletteAssetCookSettings& paletteSettings) {
    return package_cooked_voxel_asset_brick_palettes(
        voxelize_scene(scene, voxelSettings), paletteSettings);
}

CookedVoxelAssetPalettePackage cook_model_with_brick_palettes(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions,
    const VoxelizeSettings& voxelSettings,
    const BrickPaletteAssetCookSettings& paletteSettings) {
    return package_cooked_voxel_asset_brick_palettes(
        cook_model(sourcePath, importOptions, voxelSettings), paletteSettings);
}


} // namespace dve
