#include "dve/runtime_brickmap_world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1U));
    return values[index];
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::size_t bricks = 2048U;
        std::size_t edits = 1000U;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--bricks" && i + 1 < argc) bricks = std::stoull(argv[++i]);
            else if (arg == "--edits" && i + 1 < argc) edits = std::stoull(argv[++i]);
            else throw std::runtime_error("unknown argument: " + arg);
        }
        if (bricks == 0U || edits == 0U) throw std::runtime_error("bricks and edits must be positive");

        dve::VoxelObject object(900001U);
        object.reserve_bricks(bricks + 64U);
        for (std::size_t i = 0; i < bricks; ++i) {
            object.fill_brick({
                static_cast<std::int32_t>(i % 32U),
                static_cast<std::int32_t>((i / 32U) % 16U),
                static_cast<std::int32_t>(i / 512U)},
                static_cast<dve::MaterialId>(1U + (i % 4U)));
        }
        dve::PackedBrickmapScene packed;
        packed.rebuild(object);
        dve::PersistentRuntimeBrickmapWorldConfig config;
        config.indexHeapBytes = 32ULL * 1024ULL * 1024ULL;
        config.recordHeapBytes = 32ULL * 1024ULL * 1024ULL;
        config.materialHeapBytes = 32ULL * 1024ULL * 1024ULL;
        dve::PersistentRuntimeBrickmapWorld world(config);
        const dve::RuntimeBrickmapHandle handle = world.create_object({object.id(), {}, &packed});
        if (handle == dve::kInvalidRuntimeBrickmapHandle) throw std::runtime_error("initial publication failed");

        std::mt19937_64 random(0xD1E10ULL);
        std::uniform_int_distribution<std::size_t> brickDistribution(0U, bricks - 1U);
        std::uniform_int_distribution<std::uint16_t> voxelDistribution(0U, 511U);
        std::vector<double> updateMicroseconds;
        updateMicroseconds.reserve(edits);
        std::size_t plannedFullBytes{};
        std::size_t actualUploadBytes{};
        std::size_t dirtyRanges{};
        std::size_t readbackFailures{};

        for (std::size_t editIndex = 0; editIndex < edits; ++editIndex) {
            const std::size_t linearBrick = brickDistribution(random);
            const dve::BrickKey key{
                static_cast<std::int32_t>(linearBrick % 32U),
                static_cast<std::int32_t>((linearBrick / 32U) % 16U),
                static_cast<std::int32_t>(linearBrick / 512U)};
            const std::uint16_t voxel = voxelDistribution(random);
            const dve::Int3 local = dve::local_from_index_unchecked(voxel);
            const dve::Int3 global{
                key.x * 8 + local.x,
                key.y * 8 + local.y,
                key.z * 8 + local.z};
            const dve::MaterialId previous = object.material_at(global);
            const dve::MaterialId next = previous == 1U ? 2U : 1U;
            const dve::BrickApplyResult changed = object.set_voxel(global, next);
            if (!changed.changed) throw std::runtime_error("benchmark edit did not change a voxel");
            const dve::AppliedBrickEdit edit{key, changed.changedMask, changed.generation};
            (void)packed.update(object, std::span<const dve::AppliedBrickEdit>(&edit, 1U));

            plannedFullBytes += packed.index_grid().size_bytes() + packed.records().size_bytes() +
                packed.material_arena().size_bytes();
            const auto start = Clock::now();
            if (!world.update_object(handle, {object.id(), {}, &packed})) {
                throw std::runtime_error("incremental persistent publication failed");
            }
            updateMicroseconds.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
            const auto upload = world.last_upload_stats(handle);
            if (!upload) throw std::runtime_error("missing upload stats");
            actualUploadBytes += upload->total_bytes_uploaded();
            dirtyRanges += upload->dirtyRangeCount;
            const auto hash = world.readback_hash(handle);
            if (!hash || *hash != packed.readback_hash()) ++readbackFailures;
        }

        const dve::PersistentRuntimeBrickmapWorldStats stats = world.stats();
        std::cout << std::fixed << std::setprecision(4)
                  << "{\n"
                  << "  \"initial_bricks\": " << bricks << ",\n"
                  << "  \"edits\": " << edits << ",\n"
                  << "  \"update_us_p50\": " << percentile(updateMicroseconds, 0.50) << ",\n"
                  << "  \"update_us_p95\": " << percentile(updateMicroseconds, 0.95) << ",\n"
                  << "  \"update_us_p99\": " << percentile(updateMicroseconds, 0.99) << ",\n"
                  << "  \"planned_full_upload_bytes\": " << plannedFullBytes << ",\n"
                  << "  \"actual_dirty_upload_bytes\": " << actualUploadBytes << ",\n"
                  << "  \"upload_reduction_ratio\": "
                  << (plannedFullBytes == 0U ? 0.0 : 1.0 - static_cast<double>(actualUploadBytes) / static_cast<double>(plannedFullBytes)) << ",\n"
                  << "  \"dirty_ranges\": " << dirtyRanges << ",\n"
                  << "  \"relocations\": " << stats.totalRelocations << ",\n"
                  << "  \"readback_failures\": " << readbackFailures << ",\n"
                  << "  \"record_heap_fragmentation\": " << stats.recordHeap.externalFragmentation << "\n"
                  << "}\n";
        return readbackFailures == 0U ? 0 : 2;
    } catch (const std::exception& exception) {
        std::cerr << "dve_runtime_brickmap_publication_bench: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
