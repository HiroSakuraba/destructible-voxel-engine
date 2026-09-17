#include "dve/dvox.hpp"
#include "dve/runtime_scene.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct FixtureSpec {
    std::string name;
    std::size_t objects{};
    std::size_t bricksPerObject{};
};

[[nodiscard]] dve::VoxelMaterialDefinition material(
    std::string name,
    dve::Float4 color,
    float density,
    bool transparent = false,
    bool structural = true) {
    dve::VoxelMaterialDefinition result;
    result.name = std::move(name);
    result.baseColor = color;
    result.densityKilogramsPerCubicMeter = density;
    result.transparent = transparent;
    result.structural = structural;
    result.structuralStrength = structural ? 2.0F : 0.2F;
    result.fractureResistance = structural ? 1.5F : 0.25F;
    return result;
}

[[nodiscard]] std::vector<dve::VoxelMaterialDefinition> fixture_materials() {
    return {
        material("air", {0.0F, 0.0F, 0.0F, 0.0F}, 0.0F, true, false),
        material("concrete", {0.45F, 0.48F, 0.50F, 1.0F}, 2400.0F),
        material("wood", {0.38F, 0.18F, 0.07F, 1.0F}, 650.0F),
        material("steel", {0.32F, 0.36F, 0.40F, 1.0F}, 7850.0F),
        material("glass", {0.55F, 0.78F, 0.92F, 0.28F}, 2500.0F, true, false),
        material("alpha_grate", {0.16F, 0.18F, 0.20F, 1.0F}, 7800.0F, false, false),
    };
}

void fill_pattern(
    dve::CookedVoxelAsset& asset,
    std::size_t objectIndex,
    std::size_t bricksPerObject,
    std::uint64_t& occupiedVoxels) {
    const std::size_t kind = objectIndex % 10U;
    for (std::size_t brickIndex = 0; brickIndex < bricksPerObject; ++brickIndex) {
        const dve::BrickKey key{
            static_cast<std::int32_t>(brickIndex % 8U),
            static_cast<std::int32_t>((brickIndex / 8U) % 8U),
            static_cast<std::int32_t>(brickIndex / 64U),
        };
        if (kind <= 5U) {
            const dve::MaterialId id = kind <= 2U ? 1U : (kind <= 4U ? 2U : 3U);
            asset.object.fill_brick(key, id);
            occupiedVoxels += 512U;
            continue;
        }
        if (kind == 6U || kind == 7U) {
            // Thin glass wall: one two-voxel-thick slab per brick.
            for (std::int32_t z = 0; z < 2; ++z) {
                for (std::int32_t y = 0; y < 8; ++y) {
                    for (std::int32_t x = 0; x < 8; ++x) {
                        const dve::Int3 voxel{key.x * 8 + x, key.y * 8 + y, key.z * 8 + z};
                        if (asset.object.set_voxel(voxel, 4U).changed) ++occupiedVoxels;
                    }
                }
            }
            continue;
        }
        if (kind == 8U) {
            // Alpha-mask-like grate, preserving half the cells.
            for (std::int32_t z = 0; z < 8; ++z) {
                for (std::int32_t y = 0; y < 8; ++y) {
                    for (std::int32_t x = 0; x < 8; ++x) {
                        if (((x + y + z) & 1) != 0) continue;
                        const dve::Int3 voxel{key.x * 8 + x, key.y * 8 + y, key.z * 8 + z};
                        if (asset.object.set_voxel(voxel, 5U).changed) ++occupiedVoxels;
                    }
                }
            }
            continue;
        }
        // Mixed-density furniture/vehicle part: one brick contains wood and steel.
        for (std::int32_t z = 0; z < 8; ++z) {
            for (std::int32_t y = 0; y < 8; ++y) {
                for (std::int32_t x = 0; x < 8; ++x) {
                    const dve::MaterialId id = x < 6 ? 2U : 3U;
                    const dve::Int3 voxel{key.x * 8 + x, key.y * 8 + y, key.z * 8 + z};
                    if (asset.object.set_voxel(voxel, id).changed) ++occupiedVoxels;
                }
            }
        }
    }
}

[[nodiscard]] std::filesystem::path generate_fixture(
    const std::filesystem::path& root,
    const FixtureSpec& spec,
    std::uint64_t& occupiedVoxels) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path manifest = root / (spec.name + ".dvoxscene.json");
    std::ofstream json(manifest, std::ios::binary | std::ios::trunc);
    if (!json) throw std::runtime_error("unable to create authored fixture manifest");
    json << "{\n  \"format\":\"DVOXSCENE\",\n  \"version\":1,\n  \"name\":\""
         << spec.name << "\",\n  \"objects\":[\n";

    for (std::size_t i = 0; i < spec.objects; ++i) {
        const std::uint64_t id = 200000ULL + i;
        dve::CookedVoxelAsset asset(id);
        asset.voxelSizeMeters = 0.10F;
        asset.materials = fixture_materials();
        asset.object.reserve_bricks(spec.bricksPerObject);
        fill_pattern(asset, i, spec.bricksPerObject, occupiedVoxels);
        const std::string file = "authored_" + std::to_string(id) + ".dvox";
        std::string error;
        if (!dve::write_dvox(root / file, asset, {}, &error)) {
            throw std::runtime_error("unable to write authored fixture DVOX: " + error);
        }
        const float x = static_cast<float>(i % 25U) * 4.0F;
        const float z = static_cast<float>(i / 25U) * 4.0F;
        const bool anchored = (i % 10U) < 6U;
        const bool structural = (i % 10U) < 8U;
        json << "    {\"index\":" << i << ",\"id\":" << id
             << ",\"name\":\"Part" << i << "\",\"nodePath\":\"/District/Part" << i
             << "\",\"file\":\"" << file << "\",\"parent\":null,\"anchored\":"
             << (anchored ? "true" : "false") << ",\"structural\":"
             << (structural ? "true" : "false")
             << ",\"generateCollision\":true,\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,"
             << x << ",0," << z << ",1]}";
        if (i + 1U != spec.objects) json << ',';
        json << '\n';
    }
    json << "  ]\n}\n";
    if (!json) throw std::runtime_error("unable to finish authored fixture manifest");
    return manifest;
}

void run_fixture(const FixtureSpec& spec, const std::filesystem::path& base, std::size_t workers) {
    std::uint64_t occupiedVoxels{};
    const auto generateStart = Clock::now();
    const std::filesystem::path manifest = generate_fixture(base / spec.name, spec, occupiedVoxels);
    const double generationMs = std::chrono::duration<double, std::milli>(Clock::now() - generateStart).count();

    dve::RuntimeSceneStagingExecutorConfig executorConfig;
    executorConfig.workerCount = workers;
    executorConfig.maximumRetainedStagingBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    executorConfig.maximumPendingJobs = 8U;
    executorConfig.publicationBackpressureBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    dve::RuntimeSceneStagingExecutor executor(executorConfig);
    const auto stageStart = Clock::now();
    auto future = executor.submit_scene(manifest, {}, {},
        {dve::RuntimeSceneJobPriority::Critical, std::nullopt});
    dve::RuntimeSceneStageResult staged = future.get();
    const double stageMs = std::chrono::duration<double, std::milli>(Clock::now() - stageStart).count();
    if (!staged) throw std::runtime_error(std::string(dve::to_string(staged.error.code)) + ": " + staged.error.message);

    dve::PersistentRuntimeBrickmapWorldConfig rendererConfig;
    rendererConfig.indexHeapBytes = 64ULL * 1024ULL * 1024ULL;
    rendererConfig.recordHeapBytes = 64ULL * 1024ULL * 1024ULL;
    rendererConfig.materialHeapBytes = 64ULL * 1024ULL * 1024ULL;
    dve::PersistentRuntimeBrickmapWorld renderer(rendererConfig);
    dve::ReferenceRigidBodyWorld physics;
    dve::RuntimeSceneWorld world(&physics, &renderer);
    dve::RuntimeScenePublicationQueue queue({4U, 8ULL * 1024ULL * 1024ULL * 1024ULL});
    if (!queue.enqueue_scene(std::move(staged), {dve::RuntimeSceneJobPriority::Critical, std::nullopt})) {
        throw std::runtime_error("unable to enqueue authored fixture publication");
    }
    const auto publishStart = Clock::now();
    const auto publication = queue.publish_next(world);
    const double publishMs = std::chrono::duration<double, std::milli>(Clock::now() - publishStart).count();
    if (!publication || !*publication) {
        throw std::runtime_error(publication ? publication->error.message : "publication queue unexpectedly empty");
    }
    const dve::RuntimeScene* scene = world.scene(publication->sceneHandle);
    if (scene == nullptr) throw std::runtime_error("published scene missing");
    const std::uint64_t sceneHash = scene->state_hash();
    const std::size_t physicsBodies = physics.body_count();
    const dve::PersistentRuntimeBrickmapWorldStats rendererStats = renderer.stats();
    const dve::RuntimeSceneStagingExecutorStats executorStats = executor.stats();
    const auto unloadStart = Clock::now();
    if (!world.unload_scene(publication->sceneHandle)) throw std::runtime_error("unable to unload authored fixture");
    const double unloadMs = std::chrono::duration<double, std::milli>(Clock::now() - unloadStart).count();

    std::cout << std::fixed << std::setprecision(4)
              << "{\n"
              << "  \"fixture\": \"" << spec.name << "\",\n"
              << "  \"objects\": " << spec.objects << ",\n"
              << "  \"bricks_per_object\": " << spec.bricksPerObject << ",\n"
              << "  \"occupied_voxels\": " << occupiedVoxels << ",\n"
              << "  \"generation_ms\": " << generationMs << ",\n"
              << "  \"stage_ms\": " << stageMs << ",\n"
              << "  \"publish_ms\": " << publishMs << ",\n"
              << "  \"unload_ms\": " << unloadMs << ",\n"
              << "  \"scene_hash\": " << sceneHash << ",\n"
              << "  \"physics_bodies\": " << physicsBodies << ",\n"
              << "  \"renderer_objects\": " << rendererStats.live.objects << ",\n"
              << "  \"renderer_brick_records\": " << rendererStats.live.brickRecords << ",\n"
              << "  \"renderer_uploaded_bytes\": " << rendererStats.totalUploadedBytes << ",\n"
              << "  \"renderer_dirty_ranges\": " << rendererStats.totalDirtyRanges << ",\n"
              << "  \"record_heap_fragmentation\": " << rendererStats.recordHeap.externalFragmentation << ",\n"
              << "  \"executor_queue_wait_us\": " << executorStats.maximumQueueWaitMicroseconds << ",\n"
              << "  \"executor_stage_us\": " << executorStats.maximumStageMicroseconds << "\n"
              << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string fixture = "house";
        std::size_t workers = 4U;
        std::filesystem::path output = std::filesystem::temp_directory_path() / "dve_v1_10_authored";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--fixture" && i + 1 < argc) fixture = argv[++i];
            else if (arg == "--workers" && i + 1 < argc) workers = std::stoull(argv[++i]);
            else if (arg == "--output" && i + 1 < argc) output = argv[++i];
            else throw std::runtime_error("unknown argument: " + arg);
        }
        if (fixture == "house") run_fixture({"AuthoredHouse", 64U, 32U}, output, workers);
        else if (fixture == "city") run_fixture({"AuthoredCityBlock", 500U, 40U}, output, workers);
        else throw std::runtime_error("fixture must be house or city");
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_runtime_scene_authored_bench: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
