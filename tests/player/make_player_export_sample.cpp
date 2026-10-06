// Builds the editor project behind the dve_player_export_scene test:
//
//   dve_player_export_sample_generator <output-dir>
//
// Writes <output-dir>/project.dveproject, assets/crate.dmesh and scenes/main.dvescene: an
// editor scene that uses every dve_export_scene feature the runtime now understands -
//   Ground  anchored voxel slab
//   Cart    dynamic voxel body with tags and a custom "game.cart" component
//   Crate   .dmesh polygon object attached to the Cart (socket "bed")
//   Title   3D text (baked to voxels) attached to the Crate: a two-level attachment chain
//   Cloud   Gabor volume (baked to visual-only voxels)
// The CTest script exports it with dve_export_scene and runs dve_player headless on the result.
#include "dve/editor_document.hpp"

#include "support/export_scene_fixtures.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

dve::RigidTransform at(dve::Float3 position) {
    dve::RigidTransform transform;
    transform.position = position;
    return transform;
}

dve::editor::EditorObject box_object(dve::editor::EditorObjectId id, const char* name, dve::Float3 position,
                                     int x0, int y0, int z0, int x1, int y1, int z1, dve::MaterialId material) {
    dve::editor::EditorObject object(id, name);
    object.voxelSizeMeters = 0.1F;
    object.voxels = std::make_unique<dve::VoxelObject>(id);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) (void)object.voxels->set_voxel({x, y, z}, material);
    object.transform = at(position);
    return object;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dve_player_export_sample_generator <output-dir>\n";
        return 2;
    }
    using namespace dve::editor;
    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root / "assets");
    std::filesystem::create_directories(root / "scenes");
    std::ofstream(root / "project.dveproject") << "DVE_PROJECT 1\n";
    std::string error;
    const dve::CookedPolygonAsset crateMesh =
        dve::test_fixtures::make_box_polygon({0.15F, 0.15F, 0.15F}, {0.2F, 0.75F, 0.3F, 1.0F}, 3);
    if (!dve::write_dmesh(root / "assets/crate.dmesh", crateMesh, &error)) {
        std::cerr << "write crate.dmesh failed: " << error << '\n';
        return 1;
    }

    EditorDocument document("Export Sample");
    auto ground = box_object(1, "Ground", {0, 0, 0}, -15, 0, -10, 14, 0, 9, 1);
    ground.flags.anchored = true;
    document.add_object(std::move(ground));

    auto cart = box_object(2, "Cart", {-0.6F, 2.0F, -0.2F}, 0, 0, 0, 5, 2, 3, 2);
    cart.tags = {"vehicle"};
    document.add_object(std::move(cart));

    EditorObject crate(3, "Crate");
    crate.sourceAsset = "assets/crate.dmesh";
    crate.transform = at({-0.3F, 2.45F, 0.0F});
    document.add_object(std::move(crate));

    dve::Text3DStyle style;
    style.emSizeMeters = 0.5F;
    style.extrusionDepthMeters = 0.2F;
    style.faceColor = {0.95F, 0.85F, 0.15F, 1.0F};
    style.sideColor = {0.15F, 0.25F, 0.85F, 1.0F};
    auto cooked = dve::test_fixtures::cook_test_text(root / ".font", "AA", style, 4);
    std::filesystem::remove_all(root / ".font");
    if (!cooked) {
        std::cerr << "text cook failed: " << cooked.error << '\n';
        return 1;
    }
    EditorObject title(4, "Title");
    title.text3d = cooked.asset;
    title.voxelSizeMeters = 0.1F;
    title.flags.collisionEnabled = false;
    title.transform = at({-0.6F, 2.7F, 0.0F});
    document.add_object(std::move(title));

    EditorObject cloud(5, "Cloud");
    cloud.gaborVolume = dve::test_fixtures::make_gabor_blob(0.3F, 20.0F, {0.3F, 0.7F, 1.0F});
    cloud.voxelSizeMeters = 0.1F;
    cloud.transform = at({1.1F, 1.4F, 0.0F});
    document.add_object(std::move(cloud));

    if (!document.attach_object(3, 2, true, "bed", true, true, &error) ||
        !document.attach_object(4, 3, true, {}, true, true, &error)) {
        std::cerr << "attach failed: " << error << '\n';
        return 1;
    }
    dve::Component cartComponent;
    cartComponent.type = "game.cart";
    cartComponent.properties["speed"] = 2.5;
    cartComponent.properties["label"] = std::string("red");
    cartComponent.properties["wheels"] = std::int64_t{4};
    if (document.add_component(2, cartComponent, &error) == nullptr) {
        std::cerr << "add component failed: " << error << '\n';
        return 1;
    }
    const auto saved = document.save_transactional(root / "scenes/main.dvescene");
    if (!saved.success) {
        std::cerr << "save failed\n";
        return 1;
    }
    std::cout << "export sample project written to " << root.generic_string() << '\n';
    return 0;
}
