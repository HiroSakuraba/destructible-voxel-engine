#include "dve/render/dashr_live_instance.hpp"
#include "dve/rhi/null_device.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float epsilon = 1.0e-3F) {
    return std::abs(a - b) <= epsilon;
}

CookedPolygonAsset make_height_triangle() {
    CookedPolygonAsset asset;
    asset.objectId = 707U;
    VoxelMaterialDefinition material;
    material.name = "dashr-live";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.images.push_back({"height", "image/raw", 1U, 1U, {128U,128U,128U,255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"height", 0U, 0U});
    asset.materialBindings[0].height.texture = 0U;
    asset.materialBindings[0].height.texcoord = 0U;
    asset.materialBindings[0].mapping.mappingMode = MaterialMappingMode::UV0;
    asset.vertices = {
        {{0,0,0},{0,0,1},{1,0,0,1},{0.1F,0.1F},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{0.9F,0.1F},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0.1F,0.9F},{1,1,1,1},{0,0}},
    };
    asset.indices = {0U,1U,2U};
    asset.submeshes = {{"triangle",0U,3U,0U}};
    asset.bounds = {{0,0,0},{1,1,0}};
    asset.contentHash = polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)), "DASHR live fixture is invalid");
    return asset;
}

DashrAtlasShaderBytecode atlas_bytecode() {
    return {{std::byte{1}}, {std::byte{2}}, {std::byte{3}}};
}

DashrLiveSurfaceConfig config() {
    DashrLiveSurfaceConfig result;
    result.atlasResolution = 64U;
    DashrLiveMaterialConfig material;
    material.materialIndex = 0U;
    material.traceSettings.heightScale = 0.20F;
    material.traceSettings.heightReferencePlane = 0.25F;
    material.traceSettings.heightOffset = 0.03F;
    material.traceSettings.envelopePadding = 0.02F;
    result.materials.push_back(material);
    return result;
}

void test_instance_initialization_and_pose_revision() {
    rhi::NullDevice device;
    std::string error;
    auto asset = make_height_triangle();
    DashrLiveSurfaceInstance instance(device);
    DashrLiveSurfaceUpdateStats stats;
    require(instance.initialize(asset, atlas_bytecode(), config(), {}, 10U, &stats, &error),
            error.c_str());
    require(instance.ready(), "DASHR live instance did not become ready");
    require(instance.asset_content_hash() == asset.contentHash,
            "DASHR live instance lost its source asset identity");
    require(instance.pose_revision() == 10U, "initial DASHR pose revision is wrong");
    require(instance.uses_material(0U) && !instance.uses_material(1U),
            "DASHR live material routing is wrong");
    require(instance.shell() && instance.shell()->submeshes().size() == 1U,
            "DASHR live shell was not published");
    require(stats.surfaceBuild.triangles == 1U && stats.atlas.trianglesRasterized == 1U,
            "DASHR live initialization did not publish surface and atlas work");
    require(stats.shellBuild.emittedTriangles == 8U,
            "DASHR live initialization did not build the conservative prism");

    // For h in [0,1], this config maps to [-0.02, 0.18], then adds 0.02 padding.
    require(close(stats.shellBuild.minimumExtrusion, -0.04F),
            "DASHR live shell lower envelope is not conservative");
    require(close(stats.shellBuild.maximumExtrusion, 0.20F),
            "DASHR live shell upper envelope is not conservative");

    DashrLiveSurfaceUpdateStats unchanged;
    require(instance.update_pose(asset, {}, 10U, &unchanged, &error), error.c_str());
    require(unchanged.skippedUnchangedPose,
            "unchanged DASHR pose revision should not rebuild resources");

    std::vector<Float3> stretched{{0,0,0},{2,0,0},{0,1,0}};
    DashrLiveSurfaceUpdateStats moved;
    require(instance.update_pose(asset, stretched, 11U, &moved, &error), error.c_str());
    require(instance.pose_revision() == 11U, "DASHR pose revision did not advance");
    require(close(moved.surfaceBuild.maximumDistortionU, 2.0F),
            "DASHR live pose update lost the expected U stretch ratio");
    require(instance.shell()->submeshes()[0].indexCount == 24U,
            "DASHR live pose update changed shell topology");

    error.clear();
    require(!instance.update_pose(asset, stretched, 9U, nullptr, &error),
            "DASHR live instance accepted a backwards pose revision");
    require(!error.empty(), "backwards DASHR pose revision did not report an error");

    require(instance.reset(&error), error.c_str());
    require(!instance.ready(), "DASHR live reset retained published resources");
}

void test_config_rejects_shadow_mismatch_and_missing_height() {
    rhi::NullDevice device;
    std::string error;
    DashrLiveSurfaceInstance instance(device);

    auto masked = make_height_triangle();
    masked.materials[0].blendMode = MaterialBlendMode::Masked;
    masked.contentHash = polygon_asset_content_hash(masked);
    require(!instance.initialize(masked, atlas_bytecode(), config(), {}, 1U, nullptr, &error),
            "DASHR live instance accepted masked material without displaced-shadow alpha parity");

    auto missing = make_height_triangle();
    missing.materialBindings[0].height.texture.reset();
    missing.contentHash = polygon_asset_content_hash(missing);
    error.clear();
    require(!instance.initialize(missing, atlas_bytecode(), config(), {}, 1U, nullptr, &error),
            "DASHR live instance accepted a material without a height texture");
}

} // namespace

int main() {
    try {
        test_instance_initialization_and_pose_revision();
        test_config_rejects_shadow_mismatch_and_missing_height();
        std::cout << "dve_dashr_live_instance_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_dashr_live_instance_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
