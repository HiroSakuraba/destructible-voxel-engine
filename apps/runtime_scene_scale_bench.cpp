#include "dve/dvox.hpp"
#include "dve/runtime_scene.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1U));
    return values[index];
}

struct TimingSummary {
    double totalMs{};
    double p50Ms{};
    double p95Ms{};
    double p99Ms{};
    std::size_t peakRetainedBytes{};
};

[[nodiscard]] dve::VoxelMaterialDefinition make_air() {
    dve::VoxelMaterialDefinition value;
    value.name = "air";
    value.baseColor = {0.0F, 0.0F, 0.0F, 0.0F};
    value.densityKilogramsPerCubicMeter = 0.0F;
    value.structuralStrength = 0.0F;
    value.fractureResistance = 0.0F;
    value.transparent = true;
    value.structural = false;
    return value;
}

[[nodiscard]] dve::VoxelMaterialDefinition make_solid() {
    dve::VoxelMaterialDefinition value;
    value.name = "benchmark-solid";
    value.baseColor = {0.45F, 0.55F, 0.65F, 1.0F};
    value.densityKilogramsPerCubicMeter = 1600.0F;
    value.structuralStrength = 2.0F;
    value.fractureResistance = 1.5F;
    return value;
}

[[nodiscard]] std::filesystem::path generate_package(
    const std::filesystem::path& root,
    std::size_t objectCount,
    std::size_t bricksPerObject) {
    std::filesystem::create_directories(root);
    const std::filesystem::path manifest = root / "synthetic_scale.dvoxscene.json";
    std::ofstream json(manifest, std::ios::binary | std::ios::trunc);
    if (!json) throw std::runtime_error("unable to create synthetic manifest");
    json << "{\n  \"format\": \"DVOXSCENE\",\n  \"version\": 1,\n"
         << "  \"name\": \"SyntheticScale\",\n  \"objects\": [\n";

    for (std::size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        const std::uint64_t id = 100000ULL + objectIndex;
        dve::CookedVoxelAsset asset(id);
        asset.voxelSizeMeters = 0.10F;
        asset.materials = {make_air(), make_solid()};
        asset.object.reserve_bricks(bricksPerObject);
        for (std::size_t brickIndex = 0; brickIndex < bricksPerObject; ++brickIndex) {
            const std::int32_t x = static_cast<std::int32_t>(brickIndex % 16U);
            const std::int32_t y = static_cast<std::int32_t>((brickIndex / 16U) % 16U);
            const std::int32_t z = static_cast<std::int32_t>(brickIndex / 256U);
            asset.object.fill_brick({x, y, z}, 1);
        }
        const std::string file = "object_" + std::to_string(id) + ".dvox";
        std::string error;
        if (!dve::write_dvox(root / file, asset, {}, &error)) {
            throw std::runtime_error("unable to write synthetic DVOX: " + error);
        }
        const float x = static_cast<float>(objectIndex % 32U) * 2.0F;
        const float z = static_cast<float>(objectIndex / 32U) * 2.0F;
        json << "    {\"index\":" << objectIndex
             << ",\"id\":" << id
             << ",\"name\":\"Object" << objectIndex
             << "\",\"nodePath\":\"/Object" << objectIndex
             << "\",\"file\":\"" << file
             << "\",\"parent\":null,\"anchored\":"
             << ((objectIndex % 8U) == 0U ? "true" : "false")
             << ",\"structural\":true,\"generateCollision\":true,\"worldMatrix\":["
             << "1,0,0,0,0,1,0,0,0,0,1,0," << x << ",0," << z << ",1]}";
        if (objectIndex + 1U != objectCount) json << ',';
        json << '\n';
    }
    json << "  ]\n}\n";
    if (!json) throw std::runtime_error("unable to finish synthetic manifest");
    return manifest;
}

[[nodiscard]] TimingSummary benchmark_per_request(
    const std::filesystem::path& manifest,
    std::size_t workers,
    std::size_t queuedJobs,
    std::uint64_t& acceptedHash) {
    dve::RuntimeSceneWorld stagingWorld;
    dve::RuntimeSceneStageOptions options;
    options.workerCount = workers;
    std::vector<std::future<dve::RuntimeSceneStageResult>> futures;
    futures.reserve(queuedJobs);
    std::vector<double> completion;
    completion.reserve(queuedJobs);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < queuedJobs; ++i) {
        futures.push_back(stagingWorld.stage_scene_package_async(manifest, {}, options));
    }
    for (auto& future : futures) {
        dve::RuntimeSceneStageResult result = future.get();
        if (!result) throw std::runtime_error(std::string(dve::to_string(result.error.code)) + ": " + result.error.message);
        if (acceptedHash == 0U) acceptedHash = result.staging.staged_scene_hash();
        else if (acceptedHash != result.staging.staged_scene_hash()) throw std::runtime_error("per-request staging hash mismatch");
        completion.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        result.staging = {};
    }
    TimingSummary summary;
    summary.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    summary.p50Ms = percentile(completion, 0.50);
    summary.p95Ms = percentile(completion, 0.95);
    summary.p99Ms = percentile(completion, 0.99);
    return summary;
}

[[nodiscard]] TimingSummary benchmark_persistent(
    const std::filesystem::path& manifest,
    std::size_t workers,
    std::size_t queuedJobs,
    std::uint64_t expectedHash) {
    dve::RuntimeSceneStagingExecutor executor(workers, 16ULL * 1024ULL * 1024ULL * 1024ULL);
    std::vector<std::future<dve::RuntimeSceneStageResult>> futures;
    futures.reserve(queuedJobs);
    std::vector<double> completion;
    completion.reserve(queuedJobs);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < queuedJobs; ++i) futures.push_back(executor.submit_scene(manifest));
    for (auto& future : futures) {
        dve::RuntimeSceneStageResult result = future.get();
        if (!result) throw std::runtime_error(std::string(dve::to_string(result.error.code)) + ": " + result.error.message);
        if (result.staging.staged_scene_hash() != expectedHash) throw std::runtime_error("persistent staging hash mismatch");
        completion.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        result.staging = {};
    }
    const dve::RuntimeSceneStagingExecutorStats stats = executor.stats();
    TimingSummary summary;
    summary.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    summary.p50Ms = percentile(completion, 0.50);
    summary.p95Ms = percentile(completion, 0.95);
    summary.p99Ms = percentile(completion, 0.99);
    summary.peakRetainedBytes = stats.peakRetainedStagingBytes;
    return summary;
}

void write_summary(std::string_view name, const TimingSummary& value, bool trailingComma) {
    std::cout << "  \"" << name << "\": {"
              << "\"total_ms\": " << value.totalMs
              << ", \"completion_p50_ms\": " << value.p50Ms
              << ", \"completion_p95_ms\": " << value.p95Ms
              << ", \"completion_p99_ms\": " << value.p99Ms
              << ", \"peak_retained_bytes\": " << value.peakRetainedBytes << "}"
              << (trailingComma ? "," : "") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::size_t objects = 64;
        std::size_t bricksPerObject = 32;
        std::size_t queuedJobs = 4;
        std::size_t workers = 4;
        std::filesystem::path output = std::filesystem::temp_directory_path() / "dve_v1_9_scale_bench";
        bool keep = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--objects" && i + 1 < argc) objects = std::stoull(argv[++i]);
            else if (arg == "--bricks-per-object" && i + 1 < argc) bricksPerObject = std::stoull(argv[++i]);
            else if (arg == "--jobs" && i + 1 < argc) queuedJobs = std::stoull(argv[++i]);
            else if (arg == "--workers" && i + 1 < argc) workers = std::stoull(argv[++i]);
            else if (arg == "--output" && i + 1 < argc) output = argv[++i];
            else if (arg == "--keep") keep = true;
            else throw std::runtime_error("unknown argument: " + arg);
        }
        if (objects == 0U || bricksPerObject == 0U || queuedJobs == 0U) {
            throw std::runtime_error("objects, bricks-per-object, and jobs must be positive");
        }
        std::filesystem::remove_all(output);
        const auto generateStart = Clock::now();
        const std::filesystem::path manifest = generate_package(output, objects, bricksPerObject);
        const double generateMs = std::chrono::duration<double, std::milli>(Clock::now() - generateStart).count();
        std::uint64_t acceptedHash = 0U;
        const TimingSummary perRequest = benchmark_per_request(manifest, workers, queuedJobs, acceptedHash);
        const TimingSummary persistent = benchmark_persistent(manifest, workers, queuedJobs, acceptedHash);
        const std::uint64_t voxels = static_cast<std::uint64_t>(objects) *
            static_cast<std::uint64_t>(bricksPerObject) * 512ULL;

        std::cout << std::fixed << std::setprecision(4)
                  << "{\n"
                  << "  \"objects\": " << objects << ",\n"
                  << "  \"bricks_per_object\": " << bricksPerObject << ",\n"
                  << "  \"voxels\": " << voxels << ",\n"
                  << "  \"queued_jobs\": " << queuedJobs << ",\n"
                  << "  \"worker_threads\": " << workers << ",\n"
                  << "  \"package_generation_ms\": " << generateMs << ",\n"
                  << "  \"scene_hash\": " << acceptedHash << ",\n";
        write_summary("per_request_async", perRequest, true);
        write_summary("persistent_executor", persistent, false);
        std::cout << "}\n";
        if (!keep) std::filesystem::remove_all(output);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_runtime_scene_scale_bench: " << exception.what() << '\n';
        return 1;
    }
}
