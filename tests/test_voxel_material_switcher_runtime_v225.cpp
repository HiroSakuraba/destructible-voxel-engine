#include "dve/voxel_material_switcher.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<BrickPaletteMaterialRecord> materials() {
    BrickPaletteMaterialRecord stone;
    stone.globalMaterialId = 7U;
    stone.baseColor = {0.20F, 0.16F, 0.12F};
    stone.roughness = 0.9F;

    BrickPaletteMaterialRecord gold;
    gold.globalMaterialId = 42U;
    gold.baseColor = {1.00F, 0.70F, 0.10F};
    gold.roughness = 0.2F;
    gold.metallic = 1.0F;
    return {stone, gold};
}

BrickSourceSurface source_surface() {
    BrickSourceSurface surface;
    surface.brickId = 17U;
    surface.assetName = "GoldRock";
    surface.samples.push_back({{{7U, 0.30F}, {42U, 0.70F}}});
    surface.samples.push_back({{{7U, 0.80F}, {42U, 0.20F}}});
    return surface;
}

BrickPaletteSurfacePoint point() {
    BrickPaletteSurfacePoint value;
    value.worldPosition = {0.25F, 0.5F, -0.25F};
    value.worldNormal = {0.0F, 1.0F, 0.0F};
    value.assetU = 0.25F;
    value.assetV = 0.75F;
    return value;
}

std::vector<BrickPaletteShadedSample> bake_all(
    const CookedBrickPalette& palette,
    const std::vector<BrickPaletteMaterialRecord>& records) {
    std::vector<BrickPaletteShadedSample> baked;
    baked.reserve(palette.samples.size());
    for (std::size_t index = 0U; index < palette.samples.size(); ++index)
        baked.push_back(bake_brick_palette_sample(
            palette, records, index, point(), BrickPaletteShadingConfig{}));
    return baked;
}

void test_recook_transition_and_live_switching() {
    auto records = materials();
    BrickPaletteCookConfig initialConfig;
    initialConfig.maximumPaletteSlots = 2U;
    initialConfig.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    const auto initialPalette = cook_brick_palette(source_surface(), initialConfig);

    VoxelMaterialRepresentationSet representations;
    representations.bakedProperties = bake_all(initialPalette, records);
    representations.singleMaterialIds = std::vector<std::uint32_t>{42U, 7U};
    representations.canonicalSource = BrickSourceSurface{
        initialPalette.brickId, initialPalette.assetName, initialPalette.canonicalSamples};
    representations.generation = 1U;

    VoxelMaterialSwitcherState state{.activePath = VoxelMaterialRuntimePath::BakedProperties};
    const auto request = request_voxel_material_switch(
        state, representations, VoxelMaterialRuntimePath::DeferredPalette2);
    require(request.valid && request.usedFallback && request.recookRequested &&
            state.activePath == VoxelMaterialRuntimePath::BakedProperties &&
            state.pendingRecookPath == VoxelMaterialRuntimePath::DeferredPalette2,
            "missing palette did not keep baked fallback active while requesting recook");

    const auto bakedBefore = shade_active_voxel_material_sample(
        state, representations, 0U, records, point(), BrickPaletteShadingConfig{});
    std::string error;
    require(recook_voxel_material_representation(
                representations, VoxelMaterialRuntimePath::DeferredPalette2,
                VoxelMaterialPaletteOverflowPolicy::DominantMaterial, &error),
            "palette recook failed: " + error);
    require(representations.deferredPalette2.has_value(),
            "recook did not install the palette representation");
    require(representations.deferredPalette2->canonicalSamples ==
                representations.canonicalSource->samples,
            "recook did not retain the canonical per-sample source");

    const auto completed = complete_voxel_material_recook(
        state, representations, VoxelMaterialRuntimePath::DeferredPalette2);
    require(completed.valid && state.activePath == VoxelMaterialRuntimePath::DeferredPalette2 &&
            state.pendingRecookPath == VoxelMaterialRuntimePath::NoPath &&
            state.appliedGeneration == representations.generation,
            "recooked palette did not install atomically");

    records[1].baseColor.y = 0.10F;
    const auto deferredAfter = shade_active_voxel_material_sample(
        state, representations, 0U, records, point(), BrickPaletteShadingConfig{});
    require(deferredAfter.valid && bakedBefore.valid &&
            deferredAfter.baseColor.y < bakedBefore.baseColor.y - 0.30F,
            "active deferred representation did not observe the live material edit");

    const auto backToBaked = request_voxel_material_switch(
        state, representations, VoxelMaterialRuntimePath::BakedProperties);
    const auto bakedAfter = shade_active_voxel_material_sample(
        state, representations, 0U, records, point(), BrickPaletteShadingConfig{});
    require(backToBaked.completedImmediately && bakedAfter.valid &&
            std::abs(bakedAfter.baseColor.y - bakedBefore.baseColor.y) < 0.0001F,
            "switching back to baked representation did not restore the retained fallback");
}

void test_stripped_source_and_memory_accounting() {
    auto records = materials();
    BrickPaletteCookConfig config;
    config.maximumPaletteSlots = 2U;
    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    const auto palette = cook_brick_palette(source_surface(), config);

    VoxelMaterialRepresentationSet representations;
    representations.bakedProperties = bake_all(palette, records);
    representations.generation = 4U;
    VoxelMaterialSwitcherState state{.activePath = VoxelMaterialRuntimePath::BakedProperties};

    const auto result = request_voxel_material_switch(
        state, representations, VoxelMaterialRuntimePath::DeferredPalette4);
    require(result.valid && result.usedFallback && !result.recookRequested &&
            state.pendingRecookPath == VoxelMaterialRuntimePath::NoPath,
            "stripped canonical source incorrectly promised a palette recook");
    require(voxel_material_retained_bytes(representations) ==
            representations.bakedProperties->size() * sizeof(BrickPaletteShadedSample),
            "retained representation byte accounting is incorrect");

    std::string error;
    require(!recook_voxel_material_representation(
                representations, VoxelMaterialRuntimePath::DeferredPalette4,
                VoxelMaterialPaletteOverflowPolicy::DominantMaterial, &error) &&
            error.find("canonical source") != std::string::npos,
            "recook without canonical source was not rejected");

    const auto noFallback = request_voxel_material_switch(
        state, representations, VoxelMaterialRuntimePath::DeferredPalette4, false);
    require(!noFallback.valid && noFallback.activePath == VoxelMaterialRuntimePath::NoPath,
            "disabled fallback did not leave an unavailable request invalid");
}

} // namespace

int main() {
    try {
        test_recook_transition_and_live_switching();
        test_stripped_source_and_memory_accounting();
        std::cout << "voxel material switcher runtime v2.25 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
