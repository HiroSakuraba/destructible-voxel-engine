#include "dve/voxel_material_policy.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    using namespace dve;
    std::filesystem::path output = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("v224_voxel_material_policy_demo.json");

    VoxelMaterialPolicyConfig policy;
    policy.mode = VoxelMaterialMode::Hybrid;
    policy.platform = VoxelMaterialPlatformProfile::HighEndDesktop;
    policy.deferredMaximumDistance = 35.0F;
    policy.deferredMaximumLod = 1U;

    std::vector<VoxelMaterialSelectionRequest> requests;
    const auto add = [&](std::uint64_t id, std::string name, float distance, std::uint32_t lod,
                         std::uint8_t materials, bool destructible, bool modified,
                         bool pressure, VoxelMaterialCookedRepresentations cooked = VoxelMaterialCookedRepresentations{true, true, true, true}) {
        VoxelMaterialSelectionRequest request;
        request.project = policy;
        request.brick.brickId = id;
        request.brick.assetName = std::move(name);
        request.brick.cameraDistance = distance;
        request.brick.lod = lod;
        request.brick.sourceMaterialCount = materials;
        request.brick.destructible = destructible;
        request.brick.recentlyModified = modified;
        request.brick.memoryPressure = pressure;
        request.brick.cooked = cooked;
        requests.push_back(std::move(request));
    };

    add(1U, "FreshConcreteFracture", 4.0F, 0U, 2U, true, true, false);
    add(2U, "GoldRockHero", 12.0F, 0U, 4U, true, false, false);
    add(3U, "DistantMountain", 140.0F, 3U, 3U, false, false, false);
    add(4U, "MetalCrate", 9.0F, 0U, 1U, true, false, false);
    add(5U, "StreamingPressureRegion", 20.0F, 0U, 2U, true, false, true);
    add(6U, "MissingPaletteCook", 6.0F, 0U, 2U, true, true, false,
        VoxelMaterialCookedRepresentations{true, true, false, false});

    const VoxelMaterialPolicyReport report = build_voxel_material_policy_report(requests);
    std::filesystem::create_directories(output.parent_path().empty()
        ? std::filesystem::path(".") : output.parent_path());
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "could not create " << output << '\n';
        return 1;
    }
    file << voxel_material_policy_json(report);
    if (!file) {
        std::cerr << "could not write " << output << '\n';
        return 1;
    }
    std::cout << "wrote " << output << " with " << report.brickCount
              << " policy decisions, " << report.fallbackBrickCount
              << " fallbacks, and " << report.recookRequestCount << " recook request(s)\n";
    return report.invalidBrickCount == 0U ? 0 : 2;
}
