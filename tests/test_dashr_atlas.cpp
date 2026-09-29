#include "dve/render/dashr_atlas.hpp"
#include "dve/rhi/null_device.hpp"

#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

CookedPolygonAsset make_triangle() {
    CookedPolygonAsset asset;
    asset.objectId = 9001U;
    VoxelMaterialDefinition material;
    material.name = "DASHR atlas fixture";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.vertices = {
        {{0.0F,0.0F,0.0F},{0.0F,0.0F,1.0F},{1.0F,0.0F,0.0F,1.0F},
         {0.1F,0.1F},{1,1,1,1},{0,0}},
        {{1.0F,0.0F,0.0F},{0.0F,0.0F,1.0F},{1.0F,0.0F,0.0F,1.0F},
         {0.9F,0.1F},{1,1,1,1},{0,0}},
        {{0.0F,1.0F,0.0F},{0.0F,0.0F,1.0F},{1.0F,0.0F,0.0F,1.0F},
         {0.1F,0.9F},{1,1,1,1},{0,0}},
    };
    asset.indices = {0U,1U,2U};
    asset.submeshes.push_back({"dashr",0U,3U,0U});
    asset.bounds = {{0,0,0},{1,1,0}};
    asset.contentHash = polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)), "fixture asset is invalid");
    return asset;
}

DashrAtlasShaderBytecode bytecode() {
    return {{std::byte{1}}, {std::byte{2}}, {std::byte{3}}};
}

void test_scaled_surface_differentials_and_deformation_ratio() {
    auto asset = make_triangle();
    std::string error;
    DashrSurfaceMeshBuildStats restStats;
    const auto rest = build_dashr_surface_vertices(asset, {}, &restStats, &error);
    require(rest.has_value(), error.c_str());
    require(rest->size() == 3U, "DASHR surface stream vertex count is wrong");
    require(close((*rest)[0].dPduX, 1.25F), "DASHR lost U differential scale");
    require(close((*rest)[0].dPdvY, 1.25F), "DASHR lost V differential scale");
    require(close((*rest)[0].distortionU, 1.0F) &&
            close((*rest)[0].distortionV, 1.0F),
            "rest surface should have unit deformation ratios");

    std::vector<Float3> stretched;
    stretched.reserve(asset.vertices.size());
    for (const auto& vertex : asset.vertices)
        stretched.push_back({vertex.position.x * 2.0F, vertex.position.y, vertex.position.z});

    DashrSurfaceMeshBuildStats stretchedStats;
    const auto deformed = build_dashr_surface_vertices(
        asset, stretched, &stretchedStats, &error);
    require(deformed.has_value(), error.c_str());
    require(close((*deformed)[0].dPduX, 2.5F), "deformed U differential is wrong");
    require(close((*deformed)[0].dPdvY, 1.25F), "unchanged V differential drifted");
    require(close((*deformed)[0].distortionU, 2.0F, 1.0e-3F),
            "DASHR did not measure two-times U stretch");
    require(close((*deformed)[0].distortionV, 1.0F, 1.0e-3F),
            "DASHR V stretch ratio should remain one");

    rhi::NullDevice device;
    DashrSurfaceMeshMirror mirror(device);
    require(mirror.upload(asset, {}, nullptr, &error), error.c_str());
    const auto firstCapacity = mirror.stats().vertexCapacityBytes;
    require(mirror.upload(asset, stretched, nullptr, &error), error.c_str());
    require(mirror.stats().vertexCapacityBytes == firstCapacity,
            "deformed DASHR update unnecessarily reallocated its vertex buffer");
    require(mirror.stats().publications == 2U,
            "DASHR deformation stream publication count is wrong");
}

void test_resource_creation_and_updates() {
    rhi::NullDevice device;
    std::string error;
    auto asset = make_triangle();
    DashrSurfaceMeshMirror mirror(device);
    DashrSurfaceMeshBuildStats buildStats;
    require(mirror.upload(asset, {}, &buildStats, &error), error.c_str());
    require(buildStats.degenerateUvTriangles == 0U, "valid UV triangle was marked degenerate");

    DashrAtlasResources resources;
    require(create_dashr_atlas_resources(device, bytecode(), 64U, resources, &error),
            error.c_str());
    require(resources.valid(), "DASHR atlas resource set is incomplete");
    require(resources.resolution == 64U, "DASHR atlas resolution drifted");
    require(!resources.published, "new DASHR atlas should not be published yet");

    DashrAtlasUpdateStats stats;
    rhi::FenceHandle fence;
    require(record_dashr_atlas_update(device, resources, mirror, asset, stats, &fence, &error),
            error.c_str());
    require(resources.published && fence, "DASHR atlas update did not publish");
    require(stats.trianglesRasterized == 1U && stats.renderPasses == 1U,
            "DASHR atlas raster stats are wrong");
    require(stats.edgeFillDispatches == 1U && stats.textureTransitions == 8U,
            "DASHR first-update edge fill or barriers are wrong");

    // A subsequent frame must transition the sampled result back to writable states,
    // then restore the final ShaderRead contract.
    require(record_dashr_atlas_update(device, resources, mirror, asset, stats, nullptr, &error),
            error.c_str());
    require(stats.textureTransitions == 16U,
            "DASHR repeated update did not round-trip texture states");

    std::vector<Float4> seam(static_cast<std::size_t>(resources.resolution) *
                             resources.resolution, Float4{0.25F,0.75F,-1.0F,0.0F});
    seam[10U] = {0.9F,0.2F,1.0F,1.0F};
    require(upload_dashr_seam_map(device, resources, seam, &error), error.c_str());
    seam.pop_back();
    require(!upload_dashr_seam_map(device, resources, seam, &error),
            "DASHR seam upload accepted the wrong texel count");

    require(destroy_dashr_atlas_resources(device, resources, &error), error.c_str());
    require(!resources.valid(), "DASHR resource destruction left live handles");
}

void test_creation_limits() {
    rhi::NullDevice device;
    std::string error;
    DashrAtlasResources resources;
    require(!create_dashr_atlas_resources(device, bytecode(), 16U, resources, &error),
            "DASHR atlas accepted a too-small resolution");
    error.clear();
    require(!create_dashr_atlas_resources(device, {}, 64U, resources, &error),
            "DASHR atlas accepted missing shader bytecode");
}

} // namespace

int main() {
    try {
        test_scaled_surface_differentials_and_deformation_ratio();
        test_resource_creation_and_updates();
        test_creation_limits();
        std::cout << "dve_dashr_atlas_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_dashr_atlas_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
