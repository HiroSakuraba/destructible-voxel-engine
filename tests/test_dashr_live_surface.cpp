#include "dve/render/dashr_live_surface.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::render;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

CookedPolygonAsset triangle() {
    CookedPolygonAsset asset;
    asset.objectId = 908U;
    VoxelMaterialDefinition material;
    material.name = "live DASHR test";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.vertices = {
        {{0,0,0},{0,0,1},{1,0,0,1},{0.1F,0.1F},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{0.9F,0.1F},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0.1F,0.9F},{1,1,1,1},{0,0}},
    };
    asset.indices = {0U,1U,2U};
    asset.submeshes.push_back({"surface",0U,3U,0U});
    asset.bounds = {{0,0,0},{1,1,0}};
    asset.contentHash = polygon_asset_content_hash(asset);
    return asset;
}

void test_independent_pose_publication() {
    rhi::NullDevice device;
    auto asset = triangle();
    const DashrAtlasShaderBytecode code{{std::byte{1}}, {std::byte{2}}, {std::byte{3}}};
    const DashrSurfaceSettings settings{};
    std::string error;
    DashrLiveSurfaceInstance first(device), second(device);
    require(first.initialize(asset, settings, code, 64U, &error), error.c_str());
    require(second.initialize(asset, settings, code, 64U, &error), error.c_str());
    require(first.update_pose({{}, 0U}, &error), error.c_str());
    require(second.update_pose({{}, 0U}, &error), error.c_str());
    require(first.published() && second.published(), "initial poses were not published");
    require(first.update_pose({{}, 0U}, &error), error.c_str());
    require(first.atlas_updates() == 1U && first.skipped_updates() == 1U,
            "unchanged pose rebuilt the first atlas");

    std::array<Float3,3> bent{{{0,0,0},{1,0,0.25F},{0,1,0}}};
    require(first.update_pose({bent, 1U}, &error), error.c_str());
    require(first.published_revision() == 1U && second.published_revision() == 0U,
            "one instance changed the other instance's revision");
    require(first.atlas_updates() == 2U && second.atlas_updates() == 1U,
            "instance atlas updates were not independent");
    require(!first.update_pose({std::span<const Float3>(bent.data(), 2U), 2U}, &error),
            "invalid pose length was accepted");
    require(first.published_revision() == 1U,
            "rejected pose changed the published revision");
}
}

int main() {
    try {
        test_independent_pose_publication();
        std::cout << "dve_dashr_live_surface_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_dashr_live_surface_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
