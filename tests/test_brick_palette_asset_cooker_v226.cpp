#include "dve/brick_palette_asset_cooker.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

dve::VoxelMaterialDefinition material(const char* name, float r, float g, float b) {
    dve::VoxelMaterialDefinition value;
    value.name = name;
    value.baseColor = {r, g, b, 1.0F};
    value.roughness = 0.4F;
    return value;
}

} // namespace

int main() {
    try {
        using namespace dve;
        VoxelObject object(9001U);
        object.fill_brick({0, 0, 0}, 1U);
        object.set_voxel({0, 0, 0}, 2U);
        object.set_voxel({8, 0, 0}, 2U);
        object.set_voxel({9, 0, 0}, 3U);

        std::vector<VoxelMaterialDefinition> materials;
        materials.push_back(material("Air", 0.0F, 0.0F, 0.0F));
        materials.push_back(material("Stone", 0.5F, 0.5F, 0.5F));
        materials.push_back(material("Gold", 1.0F, 0.7F, 0.1F));
        materials.push_back(material("Copper", 0.7F, 0.2F, 0.1F));
        materials.push_back(material("Paint", 0.1F, 0.2F, 0.9F));

        BrickPaletteAssetCookSettings settings;
        settings.palette2Overflow = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
        settings.palette4Overflow = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
        const auto first = cook_voxel_object_brick_palettes(object, materials, settings);
        const auto second = cook_voxel_object_brick_palettes(object, materials, settings);
        require(first.dependencyKey == second.dependencyKey, "full cook dependency key is nondeterministic");
        require(first.contentHash == second.contentHash, "full cook content hash is nondeterministic");
        require(first.bricks.size() == 2U, "expected two cooked bricks");
        require(first.remap.globalMaterialIds.size() >= 3U, "global material remap was not built");
        require(first.retainedBytes > 0U && first.canonicalBytes > 0U, "retention accounting is empty");
        std::string error;
        require(validate_cooked_brick_palette_asset(first, &error), error.c_str());

        CookedBrickPaletteAsset regional = first;
        const auto* stableBefore = find_cooked_brick_materials(regional, {0, 0, 0});
        const auto* editedBefore = find_cooked_brick_materials(regional, {1, 0, 0});
        require(stableBefore != nullptr && editedBefore != nullptr, "initial brick lookup failed");
        const std::uint64_t stableKey = stableBefore->dependencyKey;
        const std::uint64_t editedKey = editedBefore->dependencyKey;

        const BrickApplyResult editResult = object.set_voxel({10, 0, 0}, 4U);
        AppliedBrickEdit edit{{1, 0, 0}, editResult.changedMask, editResult.generation};
        const auto recook = recook_voxel_object_brick_palettes(
            regional, object, materials, std::span<const AppliedBrickEdit>(&edit, 1U), settings);
        require(recook.valid && recook.recookedBricks == 1U, "regional recook did not replace one brick");
        require(recook.generation == first.generation + 1U, "regional generation did not advance");
        require(find_cooked_brick_materials(regional, {0, 0, 0})->dependencyKey == stableKey,
                "regional recook changed an unaffected brick");
        require(find_cooked_brick_materials(regional, {1, 0, 0})->dependencyKey != editedKey,
                "regional recook retained the stale dependency key");
        require(validate_cooked_brick_palette_asset(regional, &error), error.c_str());

        BrickPaletteAssetCookSettings stripped = settings;
        stripped.retention.retainBakedProperties = false;
        stripped.retention.retainSingleMaterial = false;
        stripped.retention.retainPalette2 = false;
        stripped.retention.retainCanonicalSource = false;
        const auto lean = cook_voxel_object_brick_palettes(object, materials, stripped);
        require(!lean.bricks.empty(), "lean profile produced no bricks");
        for (const auto& brick : lean.bricks) {
            require(!brick.representations.bakedProperties.has_value(), "lean profile retained baked data");
            require(!brick.representations.singleMaterialIds.has_value(), "lean profile retained single IDs");
            require(!brick.representations.deferredPalette2.has_value(), "lean profile retained palette2");
            require(!brick.representations.canonicalSource.has_value(), "lean profile retained canonical source");
        }
        require(lean.canonicalBytes == 0U, "lean canonical byte count is nonzero");

        auto changedMaterials = materials;
        changedMaterials[2].baseColor.x = 0.2F;
        require(brick_palette_asset_dependency_key(object, materials, settings) !=
                brick_palette_asset_dependency_key(object, changedMaterials, settings),
                "material edits do not invalidate the asset dependency key");

        CookedVoxelAsset ordinary(object.id());
        ordinary.voxelSizeMeters = settings.voxelSizeMeters;
        ordinary.materials = materials;
        ordinary.object = std::move(object);
        auto integrated = package_cooked_voxel_asset_brick_palettes(std::move(ordinary), settings);
        require(integrated.success, "ordinary cooked-asset packaging reported failure");
        require(integrated.asset.object.id() == integrated.brickPalettes.objectId,
                "ordinary cooked asset and palette package object IDs differ");
        require(!integrated.brickPalettes.bricks.empty(),
                "ordinary cooked asset did not receive brick-palette representations");

        std::cout << "dve_v226_brick_palette_asset_cooker_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_v226_brick_palette_asset_cooker_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
