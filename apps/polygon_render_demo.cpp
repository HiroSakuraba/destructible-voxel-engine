#include "dve/hybrid_scene_manifest.hpp"
#include "dve/render/mesh_heap.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

namespace {
using namespace dve;

CookedPolygonAsset make_panel(std::uint64_t id,
                              Float4 color,
                              MaterialShadingModel model,
                              bool transparent = false,
                              bool textured = false,
                              bool triangleLod = false) {
    CookedPolygonAsset asset;
    asset.objectId = id;

    VoxelMaterialDefinition material;
    material.name = "Panel";
    material.baseColor = color;
    material.roughness = 0.3F;
    material.shadingModel = model;
    material.clearCoat = 0.9F;
    material.clearCoatRoughness = 0.08F;
    material.foliageColor = {0.25F, 0.9F, 0.2F};
    material.foliageTransmittance = 0.65F;
    if (transparent) {
        material.blendMode = MaterialBlendMode::Translucent;
        material.transparent = true;
    }
    asset.materials.push_back(material);

    PolygonMaterialBinding binding;
    binding.doubleSided = model == MaterialShadingModel::TwoSidedFoliage;
    if (textured) {
        binding.baseColor.texture = 0U;
        PolygonImage image;
        image.name = "blue checker";
        image.mimeType = "image/raw";
        image.width = 4;
        image.height = 4;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const bool bright = ((x + y) & 1U) == 0U;
                image.rgba8.insert(image.rgba8.end(),
                                   {static_cast<std::uint8_t>(bright ? 255 : 85),
                                    static_cast<std::uint8_t>(bright ? 255 : 120),
                                    static_cast<std::uint8_t>(bright ? 255 : 220), 255});
            }
        }
        asset.images.push_back(std::move(image));
        asset.samplers.push_back({});
        asset.textures.push_back({"blue checker", 0U, 0U});
    }
    asset.materialBindings.push_back(binding);

    asset.vertices = {
        {{-0.8F, -0.8F, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        {{0.8F, -0.8F, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
        {{0.8F, 0.8F, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
        {{-0.8F, 0.8F, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}}};
    asset.indices = triangleLod ? std::vector<std::uint32_t>{0, 1, 2}
                                : std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3};
    asset.submeshes.push_back(
        {triangleLod ? "panel_lod1" : "panel", 0,
         static_cast<std::uint32_t>(asset.indices.size()), 0});
    asset.bounds = {{-0.8F, -0.8F, 0}, {0.8F, 0.8F, 0}};
    asset.contentHash = polygon_asset_content_hash(asset);
    return asset;
}


} // namespace

int main(int argc, char** argv) {
    using namespace dve;
    const std::filesystem::path output =
        argc > 1 ? argv[1] : "dve_polygon_render_v1_34.ppm";
    const std::filesystem::path json =
        argc > 2 ? argv[2] : "dve_polygon_render_v1_34.json";
    const std::filesystem::path scenePath =
        argc > 3 ? argv[3] : "dve_hybrid_scene_v1_34.dvescene";

    CookedPolygonAsset coat =
        make_panel(1, {0.03F, 0.2F, 0.95F, 1}, MaterialShadingModel::ClearCoat,
                   false, true);
    CookedPolygonAsset coatLod =
        make_panel(4, {0.03F, 0.2F, 0.95F, 1}, MaterialShadingModel::ClearCoat,
                   false, true, true);
    CookedPolygonAsset leaf =
        make_panel(2, {0.12F, 0.55F, 0.08F, 1}, MaterialShadingModel::TwoSidedFoliage);
    CookedPolygonAsset glass =
        make_panel(3, {0.7F, 0.2F, 0.9F, 0.42F}, MaterialShadingModel::StandardPBR,
                   true);

    const std::array<render::PolygonLodLevel, 1> coatLods{{{&coatLod, 100.0F}}};
    std::vector<render::PolygonRenderInstance> instances;
    instances.push_back({101, &coat,
                         make_rigid_transform({-1.0F, 0.15F, 0},
                                              quaternion_from_euler_xyz({0, 0.32F, 0})),
                         coatLods, {1, 1, 1, 1}, true});
    instances.push_back({102, &leaf,
                         make_rigid_transform({0.75F, 0.45F, -0.2F},
                                              quaternion_from_euler_xyz({0, -0.45F, 0.12F})),
                         {}, {1, 1, 1, 1}, true});
    instances.push_back({103, &glass,
                         make_rigid_transform({0.0F, -0.45F, 0.3F},
                                              quaternion_from_euler_xyz({0, 0.12F, -0.15F})),
                         {}, {1, 1, 1, 1}, true});

    render::PolygonCamera camera;
    camera.position = {0, 0.25F, 4.6F};
    camera.target = {0, 0, 0};
    RenderEnvironment environment;
    environment.sunDirection = {-0.5F, 0.9F, 1.0F};
    environment.sunIntensity = 2.8F;
    environment.skyColor = {0.12F, 0.18F, 0.28F};
    environment.groundColor = {0.08F, 0.05F, 0.03F};
    VoxelObject voxelWall(7001U);
    for (std::int32_t z = -1; z <= 0; ++z) {
        for (std::int32_t y = -1; y <= 1; ++y) {
            for (std::int32_t x = -3; x <= -2; ++x) {
                voxelWall.set_voxel({x, y, z}, 3U);
            }
        }
    }
    std::array<VoxelMaterialDefinition, 4> voxelMaterials{};
    voxelMaterials[3].name = "Authoritative voxel brick";
    voxelMaterials[3].baseColor = {0.44F, 0.19F, 0.055F, 1.0F};
    voxelMaterials[3].roughness = 0.88F;
    const render::VoxelReferenceInstance voxelInstance{
        7001U, &voxelWall, {}, voxelMaterials, true};

    render::PolygonRenderTarget combined;
    combined.resize(640, 400);
    const auto hybridStats = render::render_hybrid_reference(
        std::span<const render::VoxelReferenceInstance>(&voxelInstance, 1U),
        instances, camera, environment, combined,
        {.preserveExistingDepth = true});
    const auto& stats = hybridStats.polygons;
    std::string error;
    if (!render::write_polygon_render_ppm(output, combined, 1.1F, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    rhi::NullDevice device;
    render::ImmutableMeshHeap heap(device);
    const std::array<const CookedPolygonAsset*, 5> assets{
        &coat, &coatLod, &leaf, &glass, &coat};
    if (!heap.rebuild(assets, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const std::array<render::MeshHeapInstance, 3> gpuInstances{{
        {101, 1, instances[0].transform, 1, 0},
        {102, 2, instances[1].transform, 0, 0},
        {103, 3, instances[2].transform, 0, 0}}};
    if (!heap.upload_instances(gpuInstances, &error) || !heap.readback_matches(&error)) {
        std::cerr << error << '\n';
        return 1;
    }

    HybridSceneManifest manifest;
    manifest.name = "DVE v1.34 authoritative hybrid reference scene";
    manifest.requiredMode = GeometryBuildMode::Hybrid;
    manifest.assets = {
        {7001, "Voxel wall", GeometryKind::Voxel, "voxel_wall.dvox", 0, {}, false,
         true, {}},
        {101, "Clear coat panel", GeometryKind::Polygon, "clear_coat.dmesh",
         coat.contentHash, instances[0].transform, false, true,
         {"clear_coat_lod1.dmesh", "blue_checker.rgba"}},
        {102, "Foliage panel", GeometryKind::Polygon, "foliage.dmesh", leaf.contentHash,
         instances[1].transform, false, true, {}},
        {103, "Translucent panel", GeometryKind::Polygon, "glass.dmesh", glass.contentHash,
         instances[2].transform, false, true, {}}};
    manifest.contentHash = hybrid_scene_manifest_hash(manifest);
    if (!write_dvescene(scenePath, manifest, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::ofstream out(json);
    out << "{\n"
        << "  \"width\": 640,\n"
        << "  \"height\": 400,\n"
        << "  \"voxel_traced_rays\": " << hybridStats.voxels.tracedRays << ",\n"
        << "  \"voxel_hit_rays\": " << hybridStats.voxels.hitRays << ",\n"
        << "  \"voxel_material_fallbacks\": " << hybridStats.voxels.materialFallbacks << ",\n"
        << "  \"submitted_instances\": " << stats.submittedInstances << ",\n"
        << "  \"submitted_triangles\": " << stats.submittedTriangles << ",\n"
        << "  \"rasterized_triangles\": " << stats.rasterizedTriangles << ",\n"
        << "  \"shaded_fragments\": " << stats.shadedFragments << ",\n"
        << "  \"depth_rejected_fragments\": " << stats.depthRejectedFragments << ",\n"
        << "  \"transparent_fragments\": " << stats.transparentFragments << ",\n"
        << "  \"texture_samples\": " << stats.textureSamples << ",\n"
        << "  \"lod_selections\": " << stats.lodSelections << ",\n"
        << "  \"mesh_heap_unique_assets\": " << heap.stats().uniqueAssets << ",\n"
        << "  \"mesh_heap_deduplicated_assets\": " << heap.stats().deduplicatedAssets
        << ",\n"
        << "  \"mesh_heap_vertex_bytes\": " << heap.stats().vertexBytes << ",\n"
        << "  \"mesh_heap_index_bytes\": " << heap.stats().indexBytes << ",\n"
        << "  \"mesh_heap_instance_bytes\": " << heap.stats().instanceBytes << ",\n"
        << "  \"scene_manifest_hash\": " << manifest.contentHash << "\n"
        << "}\n";
    std::cout << "wrote " << output << " and " << json << '\n';
    return 0;
}
