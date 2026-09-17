#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

#include "dve/packed_brickmap.hpp"
#include "dve/damage.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct RayInput {
    dve::Float3 origin{};
    dve::Float3 direction{};
    float maxDistance{};
};

[[nodiscard]] double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    const std::size_t index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

[[nodiscard]] bool same_hit(
    const std::optional<dve::RayHit>& a,
    const std::optional<dve::RayHit>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->voxel == b->voxel && a->normal == b->normal && a->material == b->material &&
           std::abs(a->distance - b->distance) < 2.0e-4F;
}

} // namespace

int main() {
    using namespace dve;
    VoxelObject scene(14001);
    scene.reserve_bricks(4096);

    // Brick-authored room with sparse interior structures and fragments.
    for (int z = -12; z <= 12; ++z) {
        for (int x = -12; x <= 12; ++x) {
            scene.fill_brick({x, -2, z}, 1);
            scene.fill_brick({x, 6, z}, 2);
        }
    }
    for (int y = -1; y <= 5; ++y) {
        for (int z = -12; z <= 12; ++z) {
            scene.fill_brick({-12, y, z}, 3);
            scene.fill_brick({12, y, z}, 3);
        }
        for (int x = -11; x <= 11; ++x) {
            scene.fill_brick({x, y, -12}, 4);
            scene.fill_brick({x, y, 12}, 4);
        }
    }
    std::uint32_t randomState = 0x14DDA123U;
    auto random = [&]() {
        randomState = randomState * 1664525U + 1013904223U;
        return randomState;
    };
    for (int i = 0; i < 350; ++i) {
        const BrickKey key{
            static_cast<int>(random() % 21U) - 10,
            static_cast<int>(random() % 7U) - 1,
            static_cast<int>(random() % 21U) - 10,
        };
        if (scene.find_brick(key) == nullptr || scene.find_brick(key)->empty()) {
            scene.fill_brick(key, static_cast<MaterialId>(5U + random() % 12U));
            if ((random() & 1U) != 0U) {
                scene.set_voxel(global_from_local(key, {3, 3, 3}), kAirMaterial);
            }
        }
    }

    // Force compact non-uniform payload encodings into the GPU material arena.
    scene.fill_brick({0, 0, 0}, 7);
    scene.set_voxel({1, 1, 1}, 8); // LocalPalette4
    scene.fill_brick({1, 0, 0}, 9);
    for (int i = 0; i < 20; ++i) {
        scene.set_voxel({8 + (i & 7), (i >> 3) & 7, (i >> 6) & 7}, static_cast<MaterialId>(20 + i));
    } // Palette8

    GpuBrickMirror mirror;
    mirror.rebuild(scene);
    PackedBrickmapScene packed;
    packed.reserve(scene.brick_count() + 64U, scene.payload_bytes() + 64U * 1024U);
    const auto buildStart = Clock::now();
    packed.rebuild(scene);
    const auto buildEnd = Clock::now();
    if (!packed.validate_against(scene)) {
        std::cerr << "packed scene validation failed\n";
        return 2;
    }

    constexpr std::size_t rayCount = 100000;
    std::vector<RayInput> rays;
    rays.reserve(rayCount);
    for (std::size_t i = 0; i < rayCount; ++i) {
        const Float3 origin{
            -70.0F + static_cast<float>(random() % 14001U) / 100.0F,
            -28.0F + static_cast<float>(random() % 9201U) / 100.0F,
            -70.0F + static_cast<float>(random() % 14001U) / 100.0F,
        };
        Float3 direction{
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
        };
        if (length_squared(direction) < 1.0F) direction = {1.0F, 0.1F, 0.3F};
        rays.push_back({origin, direction, 300.0F});
    }

    std::size_t mismatches = 0;
    std::uint64_t authorityHits = 0;
    std::uint64_t packedHits = 0;
    std::uint64_t brickLookups = 0;
    std::uint64_t emptySkips = 0;
    std::uint64_t occupiedVisits = 0;
    std::uint64_t voxelSteps = 0;
    std::vector<double> packedTimes;
    packedTimes.reserve(rayCount / 100U);

    const auto oracleStart = Clock::now();
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const RayInput& ray = rays[i];
        const auto authority = raycast_voxels(scene, ray.origin, ray.direction, ray.maxDistance);
        const auto mirrorHit = raycast_gpu_brick_mirror(mirror, ray.origin, ray.direction, ray.maxDistance);
        BrickmapRayTraceStats stats;
        const auto traceStart = Clock::now();
        const auto packedHit = raycast_packed_brickmap_hdda(
            packed, ray.origin, ray.direction, ray.maxDistance, &stats);
        const auto traceEnd = Clock::now();
        if ((i % 100U) == 0U) {
            packedTimes.push_back(std::chrono::duration<double, std::micro>(traceEnd - traceStart).count());
        }
        authorityHits += authority.has_value() ? 1U : 0U;
        packedHits += packedHit.has_value() ? 1U : 0U;
        brickLookups += stats.brickLookups;
        emptySkips += stats.emptyBrickSkips;
        occupiedVisits += stats.occupiedBrickVisits;
        voxelSteps += stats.voxelSteps;
        if (!same_hit(authority, mirrorHit) || !same_hit(authority, packedHit)) {
            if (mismatches < 8U) {
                std::cerr << "mismatch ray " << i << " origin " << ray.origin.x << ',' << ray.origin.y << ',' << ray.origin.z
                          << " dir " << ray.direction.x << ',' << ray.direction.y << ',' << ray.direction.z << '\n';
                if (authority) std::cerr << " authority " << authority->voxel.x << ',' << authority->voxel.y << ',' << authority->voxel.z
                    << " normal " << authority->normal.x << ',' << authority->normal.y << ',' << authority->normal.z
                    << " mat " << int(authority->material) << " t " << std::setprecision(9) << authority->distance << '\n';
                else std::cerr << " authority miss\n";
                if (mirrorHit) std::cerr << " mirror " << mirrorHit->voxel.x << ',' << mirrorHit->voxel.y << ',' << mirrorHit->voxel.z
                    << " normal " << mirrorHit->normal.x << ',' << mirrorHit->normal.y << ',' << mirrorHit->normal.z
                    << " mat " << int(mirrorHit->material) << " t " << std::setprecision(9) << mirrorHit->distance << '\n';
                else std::cerr << " mirror miss\n";
                if (packedHit) std::cerr << " packed " << packedHit->voxel.x << ',' << packedHit->voxel.y << ',' << packedHit->voxel.z
                    << " normal " << packedHit->normal.x << ',' << packedHit->normal.y << ',' << packedHit->normal.z
                    << " mat " << int(packedHit->material) << " t " << std::setprecision(9) << packedHit->distance << '\n';
                else std::cerr << " packed miss\n";
            }
            ++mismatches;
        }
    }
    const auto oracleEnd = Clock::now();

    // Randomized edit/readback validation.
    DamageBatchWorkspace workspace;
    std::size_t updateBytes = 0;
    std::size_t updatePublished = 0;
    std::size_t updateStale = 0;
    std::vector<double> updateTimes;
    std::vector<double> validationTimes;
    updateTimes.reserve(100);
    validationTimes.reserve(100);
    const auto editStart = Clock::now();
    for (int edit = 0; edit < 100; ++edit) {
        const SphereDamageCommand command{
            {
                -64.0F + static_cast<float>(random() % 12801U) / 100.0F,
                -8.0F + static_cast<float>(random() % 5601U) / 100.0F,
                -64.0F + static_cast<float>(random() % 12801U) / 100.0F,
            },
            0.75F + static_cast<float>(random() % 500U) / 100.0F,
            static_cast<std::uint64_t>(edit),
        };
        const std::array<SphereDamageCommand, 1> commands{{command}};
        const auto updateStart = Clock::now();
        const DamageApplyReportView report = apply_damage_commands(scene, commands, workspace);
        const PackedBrickmapUpdateStats update = packed.update(scene, report.edits);
        const auto updateEnd = Clock::now();
        updateTimes.push_back(std::chrono::duration<double, std::micro>(updateEnd - updateStart).count());
        updateBytes += update.recordBytes + update.materialBytes + update.indexGridBytes;
        updatePublished += update.published;
        updateStale += update.staleDropped;
        const auto validationStart = Clock::now();
        const bool valid = packed.validate_against(scene);
        const auto validationEnd = Clock::now();
        validationTimes.push_back(std::chrono::duration<double, std::micro>(validationEnd - validationStart).count());
        if (!valid) {
            std::cerr << "post-edit packed scene validation failed\n";
            return 3;
        }
    }
    const auto editEnd = Clock::now();

    const PackedBrickmapStorageStats storage = packed.storage_stats();
    const double buildMs = std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();
    const double oracleMs = std::chrono::duration<double, std::milli>(oracleEnd - oracleStart).count();
    const double editMs = std::chrono::duration<double, std::milli>(editEnd - editStart).count();

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\n";
    std::cout << "  \"ray_count\": " << rayCount << ",\n";
    std::cout << "  \"mismatches\": " << mismatches << ",\n";
    std::cout << "  \"authority_hits\": " << authorityHits << ",\n";
    std::cout << "  \"packed_hits\": " << packedHits << ",\n";
    std::cout << "  \"scene_bricks\": " << scene.brick_count() << ",\n";
    std::cout << "  \"index_cells\": " << storage.indexCells << ",\n";
    std::cout << "  \"record_bytes\": " << storage.recordBytes << ",\n";
    std::cout << "  \"material_live_bytes\": " << storage.materialLiveBytes << ",\n";
    std::cout << "  \"material_arena_bytes\": " << storage.materialArenaBytes << ",\n";
    std::cout << "  \"build_ms\": " << buildMs << ",\n";
    std::cout << "  \"oracle_validation_ms\": " << oracleMs << ",\n";
    std::cout << "  \"sampled_trace_us_p50\": " << percentile(packedTimes, 0.50) << ",\n";
    std::cout << "  \"sampled_trace_us_p95\": " << percentile(packedTimes, 0.95) << ",\n";
    std::cout << "  \"sampled_trace_us_p99\": " << percentile(packedTimes, 0.99) << ",\n";
    std::cout << "  \"average_brick_lookups\": " << static_cast<double>(brickLookups) / rayCount << ",\n";
    std::cout << "  \"average_empty_brick_skips\": " << static_cast<double>(emptySkips) / rayCount << ",\n";
    std::cout << "  \"average_occupied_brick_visits\": " << static_cast<double>(occupiedVisits) / rayCount << ",\n";
    std::cout << "  \"average_voxel_steps\": " << static_cast<double>(voxelSteps) / rayCount << ",\n";
    std::cout << "  \"edit_sequences\": 100,\n";
    std::cout << "  \"edit_validation_total_ms\": " << editMs << ",\n";
    std::cout << "  \"edit_upload_plan_us_p50\": " << percentile(updateTimes, 0.50) << ",\n";
    std::cout << "  \"edit_upload_plan_us_p95\": " << percentile(updateTimes, 0.95) << ",\n";
    std::cout << "  \"edit_upload_plan_us_p99\": " << percentile(updateTimes, 0.99) << ",\n";
    std::cout << "  \"full_readback_validation_us_p50\": " << percentile(validationTimes, 0.50) << ",\n";
    std::cout << "  \"full_readback_validation_us_p99\": " << percentile(validationTimes, 0.99) << ",\n";
    std::cout << "  \"update_published\": " << updatePublished << ",\n";
    std::cout << "  \"update_stale_dropped\": " << updateStale << ",\n";
    std::cout << "  \"planned_upload_bytes\": " << updateBytes << ",\n";
    std::cout << "  \"readback_hash\": " << packed.readback_hash() << "\n";
    std::cout << "}\n";
    return mismatches == 0 ? 0 : 1;
}
