#include "dve/runtime_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[nodiscard]] double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1U));
    return values[index];
}

void write_percentiles(const char* name, const std::vector<double>& values, bool trailingComma) {
    std::cout << "  \"" << name << "\": {\"p50\": " << percentile(values, 0.50)
              << ", \"p95\": " << percentile(values, 0.95)
              << ", \"p99\": " << percentile(values, 0.99) << "}"
              << (trailingComma ? "," : "") << "\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path manifest = std::filesystem::path(DVE_SOURCE_DIR) / "examples" /
            "multi_object_house.dvoxscene.json";
        std::size_t iterations = 100;
        std::size_t workers = 4;
        bool deferred = false;
        bool asynchronous = true;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--iterations" && i + 1 < argc) iterations = std::stoull(argv[++i]);
            else if (arg == "--workers" && i + 1 < argc) workers = std::stoull(argv[++i]);
            else if (arg == "--deferred") deferred = true;
            else if (arg == "--synchronous") asynchronous = false;
            else if (!arg.starts_with("--")) manifest = arg;
            else throw std::runtime_error("unknown argument: " + arg);
        }
        if (iterations == 0U) throw std::runtime_error("iterations must be positive");

        std::vector<double> stageMilliseconds;
        std::vector<double> publishMilliseconds;
        std::vector<double> deferredMilliseconds;
        std::vector<double> unloadMilliseconds;
        stageMilliseconds.reserve(iterations);
        publishMilliseconds.reserve(iterations);
        deferredMilliseconds.reserve(iterations);
        unloadMilliseconds.reserve(iterations);
        std::uint64_t acceptedHash = 0;
        std::size_t objectCount = 0;
        std::size_t loadedObjects = 0;
        dve::RigidBodyCounts acceptedBodyCounts{};
        std::uint64_t totalVoxels = 0;
        std::size_t totalBricks = 0;
        std::size_t totalCollisionBoxes = 0;
        std::size_t totalConnectivityComponents = 0;
        std::size_t stagedBytes = 0;

        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            dve::ReferenceRigidBodyWorld physics;
            dve::ReferenceRuntimeBrickmapWorld renderer;
            dve::RuntimeSceneWorld world(&physics, &renderer);
            dve::RuntimeSceneLoadOptions options;
            options.loadAllObjects = !deferred;
            dve::RuntimeSceneStageOptions stageOptions;
            stageOptions.workerCount = workers;

            const auto stageStart = std::chrono::steady_clock::now();
            dve::RuntimeSceneStageResult staged;
            if (asynchronous) {
                std::future<dve::RuntimeSceneStageResult> future =
                    world.stage_scene_package_async(manifest, options, stageOptions);
                staged = future.get();
            } else {
                staged = world.stage_scene_package(manifest, options, stageOptions);
            }
            const auto stageEnd = std::chrono::steady_clock::now();
            if (!staged) {
                throw std::runtime_error(std::string(dve::to_string(staged.error.code)) + ": " + staged.error.message);
            }
            stageMilliseconds.push_back(
                std::chrono::duration<double, std::milli>(stageEnd - stageStart).count());
            if (iteration == 0U) stagedBytes = staged.staging.estimated_bytes();

            const auto publishStart = std::chrono::steady_clock::now();
            const dve::RuntimeSceneLoadResult result =
                world.publish_staged_scene(std::move(staged.staging));
            const auto publishEnd = std::chrono::steady_clock::now();
            if (!result) {
                throw std::runtime_error(std::string(dve::to_string(result.error.code)) + ": " + result.error.message);
            }
            publishMilliseconds.push_back(
                std::chrono::duration<double, std::milli>(publishEnd - publishStart).count());

            const auto deferredStart = std::chrono::steady_clock::now();
            if (deferred) {
                const dve::RuntimeScene* scene = world.scene(result.handle);
                std::vector<std::uint64_t> ids;
                for (const dve::RuntimeSceneObject& object : scene->objects()) ids.push_back(object.metadata().id);
                for (const std::uint64_t id : ids) {
                    dve::RuntimeSceneError error;
                    if (!world.load_deferred_object(result.handle, id, &error)) {
                        throw std::runtime_error(std::string(dve::to_string(error.code)) + ": " + error.message);
                    }
                }
            }
            const auto deferredEnd = std::chrono::steady_clock::now();
            deferredMilliseconds.push_back(
                std::chrono::duration<double, std::milli>(deferredEnd - deferredStart).count());

            const dve::RuntimeScene* scene = world.scene(result.handle);
            const std::uint64_t hash = scene->state_hash();
            if (iteration == 0U) {
                acceptedHash = hash;
                objectCount = scene->objects().size();
                loadedObjects = scene->loaded_object_count();
                acceptedBodyCounts = physics.body_counts();
                for (const dve::RuntimeSceneObject& object : scene->objects()) {
                    const dve::RuntimeSceneObjectStats stats = object.stats();
                    totalVoxels += stats.voxels;
                    totalBricks += stats.bricks;
                    totalCollisionBoxes += stats.collisionBoxes;
                    totalConnectivityComponents += stats.connectivityComponents;
                    if (stats.rendererReadbackHash != stats.packedBrickmapHash) {
                        throw std::runtime_error("renderer readback hash mismatch");
                    }
                }
            } else if (hash != acceptedHash) {
                throw std::runtime_error("runtime scene hash changed across repeated loads");
            }

            const auto unloadStart = std::chrono::steady_clock::now();
            if (!world.unload_scene(result.handle)) throw std::runtime_error("scene unload failed");
            const auto unloadEnd = std::chrono::steady_clock::now();
            unloadMilliseconds.push_back(
                std::chrono::duration<double, std::milli>(unloadEnd - unloadStart).count());
            if (physics.body_count() != 0U || renderer.object_count() != 0U || world.scene_count() != 0U) {
                throw std::runtime_error("scene unload leaked runtime state");
            }
        }

        std::cout << std::fixed << std::setprecision(4)
                  << "{\n"
                  << "  \"manifest\": \"" << manifest.filename().string() << "\",\n"
                  << "  \"iterations\": " << iterations << ",\n"
                  << "  \"staging_mode\": \"" << (asynchronous ? "async" : "sync") << "\",\n"
                  << "  \"workers\": " << workers << ",\n"
                  << "  \"load_mode\": \"" << (deferred ? "manifest_then_deferred" : "transactional_all") << "\",\n"
                  << "  \"objects\": " << objectCount << ",\n"
                  << "  \"loaded_objects\": " << loadedObjects << ",\n"
                  << "  \"static_bodies\": " << acceptedBodyCounts.staticBodies << ",\n"
                  << "  \"dynamic_bodies\": " << acceptedBodyCounts.dynamicBodies << ",\n"
                  << "  \"renderer_objects\": " << objectCount << ",\n"
                  << "  \"staged_bytes\": " << stagedBytes << ",\n"
                  << "  \"voxels\": " << totalVoxels << ",\n"
                  << "  \"bricks\": " << totalBricks << ",\n"
                  << "  \"collision_boxes\": " << totalCollisionBoxes << ",\n"
                  << "  \"connectivity_components\": " << totalConnectivityComponents << ",\n"
                  << "  \"scene_hash\": " << acceptedHash << ",\n";
        write_percentiles("stage_ms", stageMilliseconds, true);
        write_percentiles("publish_ms", publishMilliseconds, true);
        write_percentiles("deferred_ms", deferredMilliseconds, true);
        write_percentiles("unload_ms", unloadMilliseconds, false);
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_runtime_scene_bench: " << exception.what() << '\n';
        return 1;
    }
}
