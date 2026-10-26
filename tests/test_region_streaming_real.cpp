#include "dve/region_streaming_runtime.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
}

int main() {
    try {
        using namespace dve;
        const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) /
            "examples" / "multi_object_house.dvoxscene.json";
        ReferenceRigidBodyWorld physics;
        ReferenceRuntimeBrickmapWorld renderer;
        RuntimeSceneWorld world(&physics, &renderer);
        RuntimeSceneLoadOptions loadOptions;
        loadOptions.loadAllObjects = false;
        const auto loaded = world.load_scene_package(manifest, loadOptions);
        check(static_cast<bool>(loaded), "deferred scene reservation");
        check(world.loaded_object_count() == 0, "scene must initially be deferred");

        // The application supplies conservative region memberships until R3's
        // automatic manifest-bound indexing exists.
        RegionCatalog catalog;
        check(catalog.set_object_regions(1001, std::vector<RegionKey>{{7,0,0,0,0}}),
              "register foundation");
        check(catalog.set_object_regions(1002, std::vector<RegionKey>{{7,0,6,0,0}}),
              "register upper block");
        check(catalog.set_object_regions(1003, std::vector<RegionKey>{{7,0,12,0,0}}),
              "register furniture");

        RuntimeSceneStagingExecutor executor(2);
        RegionStreamingRuntimeConfig config;
        config.planner = {1, 2, 100, 1, 1, 0};
        config.maximumInFlight = 2;
        config.maximumPublicationsPerTick = 1;
        // R2 does not automatically evict destructible geometry or dynamic bodies.
        RegionStreamingRuntime streamer(catalog, world, executor, loaded.handle, config);

        std::uint64_t tick = 0;
        const auto load_near = [&](RegionKey camera, std::uint64_t id) {
            for (int attempt = 0; attempt < 5000; ++attempt, ++tick) {
                const auto stats = streamer.tick(camera, tick);
                check(!stats.planRejected && !stats.sceneMissing && stats.errors == 0,
                      "R2 streaming stage/publish error");
                const auto* obj = world.scene(loaded.handle)->find_object(id);
                if (obj && obj->loaded()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            throw std::runtime_error("deferred streaming did not publish before timeout");
        };
        load_near({7,0,0,0,0}, 1001);
        check(world.loaded_object_count() == 1 && renderer.object_count() == 1,
              "foundation publication");
        load_near({7,0,6,0,0}, 1002);
        check(world.loaded_object_count() == 2 && renderer.object_count() == 2,
              "upper block publication");
        check(world.scene(loaded.handle)->find_object(1001)->loaded(),
              "default residency must retain distant object");
        check(!world.scene(loaded.handle)->find_object(1003)->loaded(),
              "unvisited object must stay deferred");
        check(streamer.pending_count() == 0, "no incomplete staged work");
        check(world.scene(loaded.handle)->find_object(1001)->stats().rendererReadbackHash ==
              world.scene(loaded.handle)->find_object(1001)->stats().packedBrickmapHash,
              "GPU readback hash must match");
        check(world.unload_scene(loaded.handle), "scene unload");
        check(physics.body_count() == 0 && renderer.object_count() == 0,
              "all renderer/physics resources released");
        std::cout << "R2 real deferred staging/publication smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "R2 real smoke: " << error.what() << '\n';
        return 1;
    }
}
