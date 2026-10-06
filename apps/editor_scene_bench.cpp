// Warmed, single-threaded microbenchmarks for per-frame editor scene work:
//  - editor_scene_render_fingerprint, computed up to three times per frame to decide
//    whether the draw list, camera preview and selection diagnostics are stale;
//  - build_voxel_draw_list, rebuilt on every camera move, with and without culling
//    of buried voxels.
// Usage: dve_editor_scene_bench [objects=64] [edge=64]
#include "dve/editor_materials.hpp"
#include "dve/editor_viewport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

template <class Function>
double median_microseconds(int iterations, Function&& function, std::size_t& checksum) {
    checksum += function();  // warm-up
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        checksum += function();
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

EditorObject solid_cube(EditorObjectId id, int edge, Float3 position) {
    EditorObject object(id, "cube");
    object.voxels = std::make_unique<VoxelObject>(id);
    object.voxelSizeMeters = 0.1F;
    object.transform.position = position;
    const int bricks = (edge + kBrickDim - 1) / kBrickDim;
    for (int x = 0; x < bricks; ++x)
        for (int y = 0; y < bricks; ++y)
            for (int z = 0; z < bricks; ++z) object.voxels->fill_brick({x, y, z}, 1);
    return object;
}

} // namespace

int main(int argc, char** argv) {
    const int objectCount = argc > 1 ? std::max(1, std::atoi(argv[1])) : 64;
    const int edge = argc > 2 ? std::max(8, std::atoi(argv[2])) : 64;
    std::size_t checksum = 0;

    EditorDocument scene("scene bench");
    for (int i = 0; i < objectCount; ++i)
        scene.add_object(solid_cube(static_cast<EditorObjectId>(i + 1), edge,
                                    {static_cast<float>(i % 8) * 8.0F, 0.0F, static_cast<float>(i / 8) * 8.0F}));
    std::size_t bricks = 0;
    for (const auto& [id, object] : scene.objects()) bricks += object.voxels->brick_count();
    const double fingerprint = median_microseconds(400, [&] {
        return static_cast<std::size_t>(editor_scene_render_fingerprint(scene) & 1U);
    }, checksum);
    std::cout << "fingerprint objects=" << objectCount << " bricks=" << bricks << " median_us=" << fingerprint
              << " per_frame_x3_us=" << fingerprint * 3.0 << '\n';

    EditorDocument single("draw list bench");
    single.add_object(solid_cube(1, edge, {0.0F, 0.0F, 0.0F}));
    const float centre = 0.05F * static_cast<float>(edge);
    EditorCamera camera;
    camera.target = {centre, centre, centre};
    camera.position = {centre + 1.4F * centre * 2.0F, centre + 1.1F * centre * 2.0F, centre + 1.6F * centre * 2.0F};
    const EditorMaterialLibrary materials;
    const UiRect viewport{0, 0, 1280, 720};
    for (const bool cull : {false, true}) {
        EditorViewportSettings settings;
        settings.cullEnclosedVoxels = cull;
        std::size_t items = 0;
        int frame = 0;
        const double rebuild = median_microseconds(15, [&] {
            camera.position.x += (frame++ % 2 == 0) ? 0.01F : -0.01F;  // a camera move invalidates the list
            const auto list = build_voxel_draw_list(single, materials, camera, viewport, settings, std::set<EditorObjectId>{});
            items = list.size();
            return list.size();
        }, checksum);
        std::cout << "draw_list edge=" << edge << " cull=" << (cull ? "on" : "off") << " items=" << items
                  << " cap=" << settings.maximumDrawVoxels << " median_ms=" << rebuild / 1000.0 << '\n';
    }
    std::cout << "checksum=" << checksum << '\n';
    return 0;
}
