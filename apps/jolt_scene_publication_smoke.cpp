#include "dve/physics_jolt_backend.hpp"
#include "dve/runtime_scene.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        const std::filesystem::path manifest = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(DVE_SOURCE_DIR) / "examples" /
                "multi_object_house.dvoxscene.json";

        dve::JoltWorldConfig physicsConfig;
        physicsConfig.workerThreads = 4U;
        dve::JoltRigidBodyWorld physics(physicsConfig);
        dve::PersistentRuntimeBrickmapWorld renderer;
        dve::RuntimeSceneWorld world(&physics, &renderer);
        const dve::RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
        if (!loaded) {
            throw std::runtime_error(std::string(dve::to_string(loaded.error.code)) +
                ": " + loaded.error.message);
        }
        if (physics.body_count() != 3U || physics.dynamic_body_count() != 2U) {
            throw std::runtime_error("unexpected Jolt static/dynamic body counts after scene publication");
        }
        for (int step = 0; step < 120; ++step) physics.step(1.0F / 60.0F);
        if (!physics.last_update_succeeded()) {
            throw std::runtime_error("Jolt reported a capacity/update error during scene stepping");
        }

        const dve::RuntimeSceneHotReloadResult reloaded =
            world.hot_reload_scene(loaded.handle, manifest);
        if (!reloaded) {
            throw std::runtime_error(std::string(dve::to_string(reloaded.error.code)) +
                ": " + reloaded.error.message);
        }
        const dve::JoltPhysicsTelemetry telemetry = physics.telemetry();
        if (telemetry.failedUpdateCalls != 0U || telemetry.lastUpdateErrorBits != 0U) {
            throw std::runtime_error("Jolt telemetry recorded failed scene updates");
        }
        const auto rendererStats = renderer.stats();
        if (rendererStats.live.objects != 3U) {
            throw std::runtime_error("persistent renderer did not retain all scene objects");
        }
        if (!world.unload_scene(loaded.handle)) {
            throw std::runtime_error("scene unload failed");
        }
        if (physics.body_count() != 0U || renderer.stats().live.objects != 0U) {
            throw std::runtime_error("scene unload leaked Jolt or renderer objects");
        }

        std::cout << "jolt_scene_publication_smoke: PASS"
                  << " updates=" << telemetry.updateCalls
                  << " workers=" << telemetry.configuredWorkerThreads
                  << " uploaded_bytes=" << rendererStats.totalUploadedBytes << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "jolt_scene_publication_smoke: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
