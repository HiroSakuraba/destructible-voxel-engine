#include "dve/hybrid_geometry_world.hpp"
#include "dve/polygon_collision.hpp"
#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/rhi/null_device.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
dve::ImportedScene make_scene() {
    using namespace dve;
    ImportedScene scene;
    scene.name = "HybridPolygonCube";
    ImportedMaterial material;
    material.name = "ClearCoatBlue";
    material.baseColorFactor = {0.08F, 0.28F, 0.85F, 1.0F};
    material.metallicFactor = 0.35F;
    material.roughnessFactor = 0.22F;
    scene.materials.push_back(material);
    ImportedMesh mesh;
    mesh.name = "Quad";
    mesh.vertices = {
        {{-1.0F,0.0F,-1.0F},{0,1,0},{0,0},{1,1,1,1}},
        {{ 1.0F,0.0F,-1.0F},{0,1,0},{1,0},{1,1,1,1}},
        {{ 1.0F,0.0F, 1.0F},{0,1,0},{1,1},{1,1,1,1}},
        {{-1.0F,0.0F, 1.0F},{0,1,0},{0,1},{1,1,1,1}},
    };
    mesh.triangles = {{{0,1,2},0,0},{{0,2,3},0,0}};
    scene.meshes.push_back(mesh);
    ImportedNode node;
    node.name = "PolygonFloor";
    node.mesh = 0;
    scene.nodes.push_back(node);
    scene.roots.push_back(0);
    return scene;
}
}

int main(int argc, char** argv) {
    using namespace dve;
    const std::filesystem::path outDir = argc > 1 ? argv[1] : std::filesystem::current_path();
    std::filesystem::create_directories(outDir);
    CookedPolygonAsset mesh = cook_polygon_scene(make_scene(), {.objectId=2002});
    std::string error;
    if (!write_dmesh(outDir / "hybrid_floor.dmesh", mesh, &error)) {
        std::cerr << error << '\n'; return 1;
    }

    VoxelObject voxelObject(1001);
    voxelObject.set_voxel({0,0,0}, 1);
    PackedBrickmapScene packed;
    packed.rebuild(voxelObject);

    ReferenceRuntimeBrickmapWorld voxelWorld;
    ReferenceRuntimeMeshWorld meshWorld;
    HybridGeometryWorld hybrid(&voxelWorld, &meshWorld);
    const auto voxelHandle = hybrid.create_voxel_object({1001, make_rigid_transform({0,0,0}, {}), &packed}, &error);
    const auto meshHandle = hybrid.create_polygon_object({2002, make_rigid_transform({0,-1,0}, {}), &mesh}, &error);

    rhi::NullDevice device;
    render::MeshRhiMirror mirror(device);
    const bool mirrorOk = mirror.upload(mesh, &error) && mirror.readback_matches(mesh, &error);
    const auto staticCollision = make_polygon_static_body_desc(mesh, make_rigid_transform({0,-1,0}, {}));
    const auto stats = hybrid.stats();

    std::ofstream json(outDir / "hybrid_geometry_evidence.json", std::ios::binary | std::ios::trunc);
    json << "{\n"
         << "  \"build_mode\": \"" << to_string(compiled_geometry_build_mode()) << "\",\n"
         << "  \"voxel_supported\": " << (geometry_kind_supported(GeometryKind::Voxel)?"true":"false") << ",\n"
         << "  \"polygon_supported\": " << (geometry_kind_supported(GeometryKind::Polygon)?"true":"false") << ",\n"
         << "  \"voxel_published\": " << (voxelHandle!=kInvalidRuntimeGeometryHandle?"true":"false") << ",\n"
         << "  \"polygon_published\": " << (meshHandle!=kInvalidRuntimeGeometryHandle?"true":"false") << ",\n"
         << "  \"objects\": " << stats.objects << ",\n"
         << "  \"vertices\": " << mesh.vertices.size() << ",\n"
         << "  \"triangles\": " << mesh.indices.size()/3U << ",\n"
         << "  \"rhi_readback\": " << (mirrorOk?"true":"false") << ",\n"
         << "  \"collision_proxy\": " << (staticCollision.has_value()?"true":"false") << "\n"
         << "}\n";
    if (!json || !mirrorOk) { std::cerr << error << '\n'; return 1; }
    std::cout << "hybrid geometry demo: " << stats.voxelObjects << " voxel, "
              << stats.polygonObjects << " polygon objects\n";
    return 0;
}
