#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "dve/connectivity.hpp"
#include "dve/damage.hpp"
#include "dve/derived_jobs.hpp"

namespace {
using Clock = std::chrono::steady_clock;

template <class Fn>
double milliseconds(Fn&& fn) {
    const auto start = Clock::now();
    fn();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}
}

int main() {
    using namespace dve;
    const AnchorPredicate anchor = [](Int3 voxel) { return voxel.z == 0; };
    const AnchorMaskProvider anchorMasks = [](BrickKey key) {
        Bitset512 mask;
        if (key.z == 0) mask.words[0] = ~std::uint64_t{0};
        return mask;
    };

    // Bridge-collapse path: repeated removal of one-voxel links between 8^3 masses.
    VoxelObject bridge(1001);
    constexpr int massCount = 12;
    for (int mass = 0; mass < massCount; ++mass) {
        const int baseX = mass * 10;
        for (int z = 0; z < 4; ++z) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 8; ++x) bridge.set_voxel({baseX + x, y, z + (mass == 0 ? 0 : 1)}, 1);
            }
        }
        if (mass + 1 < massCount) {
            bridge.set_voxel({baseX + 8, 0, 2}, 1);
            bridge.set_voxel({baseX + 9, 0, 2}, 1);
        }
    }
    IncrementalConnectivityCache bridgeCache;
    bridgeCache.initialize_with_anchor_masks(bridge, anchorMasks);
    std::size_t bridgeGraphNodes = 0;
    std::size_t bridgeComponents = 0;
    double bridgeUpdateMs = 0;
    for (int mass = 0; mass + 1 < massCount; ++mass) {
        const Int3 voxel{mass * 10 + 8, 0, 2};
        BrickMutation cut;
        cut.removeMask.set(voxel_index_unchecked(local_voxel_from_global(voxel)));
        const AppliedBrickEdit edit = bridge.apply(brick_key_from_voxel(voxel), cut);
        const std::array<AppliedBrickEdit, 1> edits{edit};
        ConnectivityUpdateStats stats;
        bridgeUpdateMs += milliseconds([&] { stats = bridgeCache.update_with_anchor_masks(bridge, edits, anchorMasks); });
        bridgeGraphNodes += stats.graphNodesVisited;
        bridgeComponents = bridgeCache.components().size();
    }

    // Swiss-cheese path: many local holes and certificate-rate telemetry.
    VoxelObject swiss(1002);
    for (int z = 0; z < 4; ++z) for (int y = 0; y < 4; ++y) for (int x = 0; x < 6; ++x) swiss.fill_brick({x,y,z}, 2);
    IncrementalConnectivityCache swissCache;
    swissCache.initialize_with_anchor_masks(swiss, anchorMasks);
    std::uint32_t randomState = 0x12345678U;
    auto random = [&] { randomState = randomState * 1664525U + 1013904223U; return randomState; };
    std::size_t swissRecomputed = 0;
    std::size_t swissStable = 0;
    std::size_t swissGraphNodes = 0;
    double swissMs = 0;
    for (int step = 0; step < 500; ++step) {
        const Int3 voxel{static_cast<int>(random() % 48U), static_cast<int>(random() % 32U), static_cast<int>(random() % 32U)};
        BrickMutation mutation;
        mutation.removeMask.set(voxel_index_unchecked(local_voxel_from_global(voxel)));
        const AppliedBrickEdit edit = swiss.apply(brick_key_from_voxel(voxel), mutation);
        if (!edit.changedMask.any()) continue;
        const std::array<AppliedBrickEdit,1> edits{edit};
        ConnectivityUpdateStats stats;
        swissMs += milliseconds([&] { stats = swissCache.update_with_anchor_masks(swiss, edits, anchorMasks); });
        swissRecomputed += stats.bricksRecomputed;
        swissStable += stats.topologyStableBricks;
        swissGraphNodes += stats.graphNodesVisited;
    }

    // Maximum authored blast bound.
    VoxelObject blast(1003);
    for (int z = 0; z < 6; ++z) for (int y = 0; y < 8; ++y) for (int x = 0; x < 12; ++x) blast.fill_brick({x,y,z}, 3);
    DamageBatchWorkspace workspace;
    workspace.reserve(4, 512, 512);
    DamageApplyReportView blastReport;
    const std::array<SphereDamageCommand,1> blastCommand{{{{48.0F,32.0F,24.0F},20.0F,1}}};
    const double blastMs = milliseconds([&] { blastReport = apply_damage_commands(blast, blastCommand, workspace); });

    // Parallel derived-work throughput over a large dirty set.
    VoxelObject surfaces(1004);
    std::vector<BrickKey> keys;
    for (int z = 0; z < 6; ++z) for (int y = 0; y < 8; ++y) for (int x = 0; x < 12; ++x) {
        const BrickKey key{x,y,z};
        surfaces.fill_brick(key, static_cast<MaterialId>(1 + (x+y+z)%4));
        surfaces.set_voxel(global_from_local(key,{3,3,3}), kAirMaterial);
        keys.push_back(key);
    }
    std::uint64_t serialFaces = 0;
    const double serialMs = milliseconds([&] {
        for (BrickKey key : keys) {
            const BrickFaceMasks masks = extract_brick_face_masks(surfaces,key);
            for (const Bitset512& mask : masks) serialFaces += mask.count();
        }
    });
    JobSystem jobs(7);
    std::vector<BrickFaceMaskResult> parallelResults;
    const double parallelMs = milliseconds([&] { parallelResults = extract_face_masks_parallel(surfaces, keys, jobs); });
    std::uint64_t parallelFaces = 0;
    for (const auto& result : parallelResults) for (const Bitset512& mask : result.masks) parallelFaces += mask.count();

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "{\n";
    std::cout << "  \"bridge_collapse\": {\"cuts\": " << massCount-1
              << ", \"update_ms_total\": " << bridgeUpdateMs
              << ", \"graph_nodes_visited\": " << bridgeGraphNodes
              << ", \"components_after\": " << bridgeComponents << "},\n";
    const double certificateRate = swissRecomputed == 0 ? 1.0 : static_cast<double>(swissStable)/static_cast<double>(swissRecomputed);
    std::cout << "  \"swiss_cheese\": {\"updates\": " << swissRecomputed
              << ", \"certificate_rate\": " << certificateRate
              << ", \"graph_nodes_visited\": " << swissGraphNodes
              << ", \"update_ms_total\": " << swissMs << "},\n";
    std::cout << "  \"max_blast\": {\"edit_ms\": " << blastMs
              << ", \"removed_voxels\": " << blastReport.removedVoxelCount
              << ", \"dirty_bricks\": " << blastReport.edits.size() << "},\n";
    std::cout << "  \"derived_face_masks\": {\"bricks\": " << keys.size()
              << ", \"serial_ms\": " << serialMs
              << ", \"scheduled_ms\": " << parallelMs
              << ", \"speedup\": " << (parallelMs > 0.0 ? serialMs/parallelMs : 0.0)
              << ", \"parallel_dispatched\": "
              << (should_parallelize_derived_bricks(keys.size(), jobs.worker_count()) ? "true" : "false")
              << ", \"face_counts_equal\": " << (serialFaces == parallelFaces ? "true" : "false") << "}\n";
    std::cout << "}\n";
    return (bridgeCache.validate(bridge) && swissCache.validate(swiss) && serialFaces == parallelFaces) ? 0 : 1;
}
