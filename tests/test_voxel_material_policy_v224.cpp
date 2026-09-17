#include "dve/voxel_material_policy.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

VoxelMaterialSelectionRequest base_request() {
    VoxelMaterialSelectionRequest request;
    request.project.mode = VoxelMaterialMode::Hybrid;
    request.project.platform = VoxelMaterialPlatformProfile::HighEndDesktop;
    request.project.deferredMaximumDistance = 40.0F;
    request.project.deferredMaximumLod = 1U;
    request.project.maximumPaletteSlots = 4U;
    request.brick.brickId = 10U;
    request.brick.assetName = "ConcreteWall";
    request.brick.cameraDistance = 8.0F;
    request.brick.lod = 0U;
    request.brick.sourceMaterialCount = 2U;
    request.brick.dominantMaterialWeight = 0.65F;
    request.brick.destructible = true;
    request.brick.canonicalMaterialMembershipAvailable = true;
    request.brick.cooked = {true, true, true, true};
    return request;
}

void test_explicit_modes_and_preview() {
    auto request = base_request();
    request.project.mode = VoxelMaterialMode::BakedProperties;
    auto decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "baked mode did not select conventional properties");

    request.project.mode = VoxelMaterialMode::SingleMaterial;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::SingleMaterial,
            "single-material mode did not select one material ID");

    request.project.mode = VoxelMaterialMode::DeferredPalette;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::DeferredPalette2,
            "two-material deferred mode did not select palette2");

    request.brick.sourceMaterialCount = 4U;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::DeferredPalette4,
            "four-material deferred mode did not select palette4");

    request.editorPreviewMode = VoxelMaterialMode::BakedProperties;
    decision = select_voxel_material_path(request);
    require(decision.requestedMode == VoxelMaterialMode::BakedProperties &&
            decision.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "editor preview did not override the project mode");
}

void test_hybrid_distance_platform_and_dynamic_selection() {
    auto request = base_request();
    auto decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::DeferredPalette2,
            "near destructible brick should retain deferred identity");

    request.brick.cameraDistance = 100.0F;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "distant brick should use baked properties");

    request.brick.cameraDistance = 4.0F;
    request.brick.sourceMaterialCount = 1U;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::SingleMaterial,
            "one-material brick should use single-material path");

    request.brick.sourceMaterialCount = 2U;
    request.brick.destructible = false;
    request.project.platform = VoxelMaterialPlatformProfile::Mobile;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "ordinary mobile brick should use conventional baking");

    request.brick.recentlyModified = true;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::DeferredPalette2,
            "recently modified mobile brick should temporarily retain deferred identity");

    request.project.platform = VoxelMaterialPlatformProfile::DedicatedServer;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::SingleMaterial &&
            decision.estimatedTextureSamples == 5U,
            "server profile did not select stripped single-material metadata");
}

void test_fallback_recook_and_overflow() {
    auto request = base_request();
    request.project.mode = VoxelMaterialMode::DeferredPalette;
    request.brick.cooked.deferredPalette2 = false;
    auto decision = select_voxel_material_path(request);
    require(decision.usedFallback && decision.recookRequested &&
            decision.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "missing deferred representation did not use safe fallback and request recook");

    request.brick.canonicalMaterialMembershipAvailable = false;
    decision = select_voxel_material_path(request);
    require(decision.usedFallback && !decision.recookRequested && !decision.runtimeSwitchAvailable,
            "irreversible baked source incorrectly promised recooking/runtime switching");

    request = base_request();
    request.project.mode = VoxelMaterialMode::DeferredPalette;
    request.project.maximumPaletteSlots = 4U;
    request.brick.sourceMaterialCount = 7U;
    request.project.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::SingleMaterial,
            "dominant-material overflow policy failed");

    request.project.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::BakeProperties;
    decision = select_voxel_material_path(request);
    require(decision.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "baked overflow policy failed");

    request.project.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::Reject;
    decision = select_voxel_material_path(request);
    require(!decision.valid && decision.runtimePath == VoxelMaterialRuntimePath::NoPath,
            "reject overflow policy did not produce an invalid decision");
}

void test_asset_override_and_deterministic_report() {
    auto near = base_request();
    near.brick.brickId = 2U;
    near.asset.mode = VoxelMaterialMode::SingleMaterial;
    near.asset.allowRuntimeSwitching = false;
    auto decision = select_voxel_material_path(near);
    require(decision.runtimePath == VoxelMaterialRuntimePath::SingleMaterial &&
            !decision.runtimeSwitchAvailable,
            "asset policy override was ignored");

    auto far = base_request();
    far.brick.brickId = 1U;
    far.brick.cameraDistance = 80.0F;
    auto four = base_request();
    four.brick.brickId = 3U;
    four.brick.sourceMaterialCount = 4U;

    std::vector<VoxelMaterialSelectionRequest> a{near, far, four};
    std::vector<VoxelMaterialSelectionRequest> b{four, near, far};
    const auto reportA = build_voxel_material_policy_report(a);
    const auto reportB = build_voxel_material_policy_report(b);
    require(reportA.contentHash == reportB.contentHash,
            "policy report hash depends on request ordering");
    require(reportA.brickCount == 3U && reportA.bakedBrickCount == 1U &&
            reportA.singleMaterialBrickCount == 1U && reportA.deferredPalette4BrickCount == 1U,
            "policy report counters are incorrect");
    require(reportA.decisions.front().brickId == 1U && reportA.decisions.back().brickId == 3U,
            "policy report ordering is unstable");
    const std::string json = voxel_material_policy_json(reportA);
    require(json.find("deferred_palette4_bricks") != std::string::npos &&
            json.find("ConcreteWall") != std::string::npos,
            "policy JSON omitted required evidence");
}

} // namespace

int main() {
    try {
        test_explicit_modes_and_preview();
        test_hybrid_distance_platform_and_dynamic_selection();
        test_fallback_recook_and_overflow();
        test_asset_override_and_deterministic_report();
        std::cout << "voxel material policy v2.24 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
