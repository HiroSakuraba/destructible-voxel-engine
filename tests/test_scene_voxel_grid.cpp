// SceneVoxelGrid and voxel_surface_mesher contracts (CPU only, no Vulkan):
//  * grid cells hold the material of the object voxel under the cell centre, for translated and
//    rotated objects;
//  * update() is incremental: an unchanged scene is a no-op, moving one object rasterizes only it,
//    editing voxels invalidates that object;
//  * the greedy mesher merges a solid cube to 6 quads and covers exactly the exposed faces.
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/render/scene_voxel_grid.hpp"
#include "dve/render/voxel_surface_mesher.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_textures.hpp"

namespace {

using namespace dve;
using namespace dve::render;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void fill_box(VoxelObject& object, int sx, int sy, int sz, MaterialId material) {
    for (int x = 0; x < sx; ++x)
        for (int y = 0; y < sy; ++y)
            for (int z = 0; z < sz; ++z) (void)object.set_voxel({x, y, z}, material);
}

float triangle_area(const CookedPolygonAsset& asset) {
    float area = 0.0F;
    for (std::size_t i = 0; i + 2U < asset.indices.size(); i += 3U) {
        const auto& a = asset.vertices[asset.indices[i]].position;
        const auto& b = asset.vertices[asset.indices[i + 1U]].position;
        const auto& c = asset.vertices[asset.indices[i + 2U]].position;
        const Float3 u{b.x - a.x, b.y - a.y, b.z - a.z};
        const Float3 v{c.x - a.x, c.y - a.y, c.z - a.z};
        const Float3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        area += 0.5F * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    }
    return area;
}

void mesher_tests() {
    VoxelObject cube(1);
    fill_box(cube, 4, 4, 4, 3);
    VoxelSurfaceMeshOptions options;
    options.cellSizeMeters = {0.1F, 0.1F, 0.1F};
    CookedPolygonAsset greedy, naive;
    VoxelSurfaceMeshStats stats;
    std::string error;
    require(build_voxel_surface_mesh(cube, {}, options, greedy, &stats, &error), error);
    require(stats.exposedVoxelFaces == 96U, "4^3 cube exposes 96 voxel faces");
    require(stats.quads == 6U, "solid cube must merge to 6 quads, got " + std::to_string(stats.quads));
    options.greedy = false;
    require(build_voxel_surface_mesh(cube, {}, options, naive, &stats, &error), error);
    require(stats.quads == 96U, "unmerged cube has one quad per voxel face");
    const float expected = 6.0F * 0.4F * 0.4F;
    require(std::abs(triangle_area(greedy) - expected) < 1.0e-4F, "greedy cube area wrong");
    require(std::abs(triangle_area(naive) - expected) < 1.0e-4F, "unmerged cube area wrong");

    // One voxel punched out of a 1-thick wall: area gains the 4 inner walls, loses nothing on net
    // (front/back each lose a voxel face but the hole exposes the same two sides of the neighbours).
    VoxelObject wall(2);
    fill_box(wall, 8, 8, 1, 3);
    (void)wall.set_voxel({3, 3, 0}, kAirMaterial);
    options.greedy = true;
    CookedPolygonAsset holed;
    require(build_voxel_surface_mesh(wall, {}, options, holed, &stats, &error), error);
    const float cell = 0.01F;
    const float wallArea = 2.0F * 64.0F * cell + 4.0F * 8.0F * cell * 1.0F; // front+back+rim
    const float holeArea = -2.0F * cell + 4.0F * cell;                      // lose 2 faces, gain 4
    require(std::abs(triangle_area(holed) - (wallArea + holeArea)) < 1.0e-4F, "holed wall area wrong");

    std::vector<VoxelMaterialDefinition> mapped(4);
    auto image = make_voxel_image("map", 1, 1, {255, 128, 64, 255}, VoxelImageRole::Color);
    mapped[3].baseColorMap = image;
    mapped[3].ormMap = image;
    mapped[3].emissiveMap = image;
    mapped[3].textureTileMeters = 0.25F;
    CookedPolygonAsset textured;
    require(build_voxel_surface_mesh(cube, mapped, options, textured, &stats, &error), error);
    require(textured.materialBindings.size() == 4U, "material IDs are preserved");
    const auto& binding = textured.materialBindings[3];
    require(binding.baseColor.texture && binding.metallicRoughness.texture && binding.emissive.texture,
            "voxel texture maps must survive conversion to GPU material bindings");
    require(textured.images.size() == 3U && textured.textures.size() == 3U,
            "all three voxel maps must be included in the mesh asset");
    require(binding.mapping.mappingMode == MaterialMappingMode::ObjectTriplanar &&
            std::abs(binding.mapping.triplanarScale - 4.0F) < 1.0e-5F,
            "voxel texture mapping must retain object-space metre scale");

    mapped[3].baseColorMap.reset(); mapped[3].ormMap.reset(); mapped[3].emissiveMap.reset();
    mapped[3].normalMap = image; mapped[3].normalMapTileMeters = 0.5F;
    mapped[3].normalMapStrength = 0.7F;
    require(build_voxel_surface_mesh(cube, mapped, options, textured, &stats, &error), error);
    require(textured.materialBindings[3].normal.texture &&
            std::abs(textured.materialBindings[3].normalScale - 0.7F) < 1.0e-5F &&
            std::abs(textured.materialBindings[3].mapping.triplanarScale - 2.0F) < 1.0e-5F,
            "normal map and its independent projection must survive voxel meshing");
    mapped[3].baseColorMap = image; mapped[3].textureTileMeters = 0.25F;
    require(build_voxel_surface_mesh(cube, mapped, options, textured, &stats, &error), error);
    require(!textured.materialBindings[3].normal.texture,
            "different normal and color scales must not silently use the wrong projection");

    mapped[3].baseColorMap.reset(); mapped[3].normalMap.reset();
    mapped[1].baseColor = {0.0F, 1.0F, 0.0F, 1.0F};
    mapped[3].layers.push_back({1U, 0.5F});
    require(build_voxel_surface_mesh(cube, mapped, options, textured, &stats, &error), error);
    require(textured.submeshes[0].materialIndex == 3U && textured.materials.size() == 4U &&
            textured.materials[3].layers[0].sourceMaterial == 1U && textured.materials[1].baseColor.y == 1.0F,
            "voxel layers must retain their original source material IDs");

    VoxelObject empty(3);
    CookedPolygonAsset none;
    require(build_voxel_surface_mesh(empty, {}, options, none, &stats, &error) && none.vertices.empty(),
            "empty object yields no vertices");
    options.cellSizeMeters = {0.0F, 0.1F, 0.1F};
    require(!build_voxel_surface_mesh(cube, {}, options, none, &stats, &error), "zero cell size is rejected");
}

SceneGridObject make_object(std::uint64_t id, const VoxelObject& voxels, RigidTransform transform) {
    SceneGridObject object;
    object.id = id;
    object.voxels = &voxels;
    object.transform = transform;
    return object;
}

void grid_tests() {
    VoxelObject a(10), b(11);
    fill_box(a, 8, 8, 8, 3);
    fill_box(b, 6, 6, 6, 5);
    RigidTransform ta{{0.0F, 0.0F, 0.0F}, {}};
    RigidTransform tb{{2.0F, 0.0F, 0.0F}, quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, 0.6F)};

    SceneVoxelGridSettings settings;
    settings.cellMeters = 0.1F;
    settings.alignmentCells = 4U;
    SceneVoxelGrid grid;
    std::vector<SceneGridObject> objects{make_object(1, a, ta), make_object(2, b, tb)};
    require(grid.update(objects, settings, {1.0F, 0.4F, 0.4F}), "first update must report a change");
    require(!grid.empty() && grid.last_rasterized_objects() == 2U, "first update rasterizes both objects");
    require(grid.palette().size() >= 2U, "palette must contain both materials");

    // Axis-aligned object: cell lookup equals the direct voxel lookup.
    for (float x = 0.05F; x < 0.8F; x += 0.1F)
        for (float y = 0.05F; y < 0.8F; y += 0.1F)
            require(grid.cell_at_world({x, y, 0.45F}) != 0U, "inside object a must be solid");
    require(grid.cell_at_world({0.45F, 0.45F, 0.95F}) == 0U, "above object a must be air");
    require(grid.cell_at_world({-5.0F, 0.0F, 0.0F}) == 0U, "outside the grid is air");

    // Rotated object: a point mapped into object space through the inverse transform must agree.
    std::size_t checked = 0, mismatches = 0;
    for (int ix = 0; ix < 6; ++ix)
        for (int iy = 0; iy < 6; ++iy)
            for (int iz = 0; iz < 6; ++iz) {
                const Float3 local{(static_cast<float>(ix) + 0.5F) * 0.1F, (static_cast<float>(iy) + 0.5F) * 0.1F,
                                   (static_cast<float>(iz) + 0.5F) * 0.1F};
                const Float3 world = transform_point(tb, local);
                ++checked;
                if (grid.cell_at_world(world) == 0U) ++mismatches;
            }
    require(mismatches * 10U <= checked, "rotated object lost more than 10% of its voxel centres");

    // In-place material edits must refresh traced lighting even when the table address is stable.
    std::vector<VoxelMaterialDefinition> materials(6);
    materials[3].baseColor = {1, 0, 0, 1};
    objects[0].materials = materials;
    require(grid.update(objects, settings, {1.0F, 0.4F, 0.4F}), "new material table changes grid");
    auto slot = grid.cell_at_world({0.45F, 0.45F, 0.45F});
    require(slot != 0 && grid.palette()[slot - 1].albedo.x == 1.0F, "red material uploaded");
    materials[3].baseColor = {0, 1, 0, 1};
    require(grid.update(objects, settings, {1.0F, 0.4F, 0.4F}), "material edit changes grid");
    slot = grid.cell_at_world({0.45F, 0.45F, 0.45F});
    require(grid.palette()[slot - 1].albedo.y == 1.0F, "edited green material uploaded");

    // Unchanged scene: nothing to do.
    require(!grid.update(objects, settings, {1.0F, 0.4F, 0.4F}), "identical scene must not report a change");
    require(grid.last_rasterized_objects() == 0U, "identical scene must come from cache");

    // Moving only b re-rasterizes only b.
    objects[1].transform.position.x = 2.02F;   // stays inside the aligned grid window
    require(grid.update(objects, settings, {1.0F, 0.4F, 0.4F}), "moved object must report a change");
    require(grid.last_rasterized_objects() == 1U, "moving one object rasterizes one object");

    // Editing voxels of a invalidates a.
    (void)a.set_voxel({0, 0, 0}, kAirMaterial);
    require(grid.update(objects, settings, {1.0F, 0.4F, 0.4F}), "edited object must report a change");
    require(grid.last_rasterized_objects() == 1U, "editing one object rasterizes one object");
    require(grid.cell_at_world({0.05F, 0.05F, 0.05F}) == 0U, "carved voxel must be air in the grid");
}

} // namespace

int main() {
    try {
        mesher_tests();
        grid_tests();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "scene voxel grid tests passed\n";
    return 0;
}
