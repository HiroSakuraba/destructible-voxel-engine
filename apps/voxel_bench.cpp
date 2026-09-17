#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

#include "dve/connectivity.hpp"
#include "dve/damage.hpp"
#include "dve/surface.hpp"
#include "dve/gpu_brick_mirror.hpp"

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = p * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(position);
    const auto high = std::min(low + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(low);
    return values[low] * (1.0 - fraction) + values[high] * fraction;
}

[[nodiscard]] bool same_connectivity(
    const dve::ConnectivitySnapshot& a,
    const dve::ConnectivitySnapshot& b) {
    if (a.brickData.size() != b.brickData.size() || a.components.size() != b.components.size()) return false;
    auto ita = a.brickData.begin();
    auto itb = b.brickData.begin();
    for (; ita != a.brickData.end(); ++ita, ++itb) {
        if (ita->first != itb->first || ita->second.generation != itb->second.generation ||
            ita->second.components != itb->second.components) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.components.size(); ++i) {
        const auto& ca = a.components[i];
        const auto& cb = b.components[i];
        if (ca.nodes != cb.nodes || ca.voxelCount != cb.voxelCount || ca.minVoxel != cb.minVoxel ||
            ca.maxVoxel != cb.maxVoxel || ca.anchored != cb.anchored) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    using namespace dve;
    VoxelObject world(1);
    constexpr int bricksX = 12;
    constexpr int bricksY = 8;
    constexpr int bricksZ = 6;
    world.reserve_bricks(static_cast<std::size_t>(bricksX * bricksY * bricksZ));
    world.reserve_payloads(128, 16, 16);
    for (int z = 0; z < bricksZ; ++z) {
        for (int y = 0; y < bricksY; ++y) {
            for (int x = 0; x < bricksX; ++x) {
                world.fill_brick({x, y, z}, static_cast<MaterialId>(1 + ((x + y + z) % 4)));
            }
        }
    }

    const AnchorPredicate anchor = [](Int3 voxel) { return voxel.z == 0; };
    const AnchorMaskProvider anchorMasks = [](BrickKey key) {
        Bitset512 mask;
        if (key.z == 0) mask.words[0] = ~std::uint64_t{0};
        return mask;
    };
    IncrementalConnectivityCache connectivityCache;
    const auto initializeStart = Clock::now();
    connectivityCache.initialize_with_anchor_masks(world, anchorMasks);
    const auto initializeEnd = Clock::now();
    const double initializeMs =
        std::chrono::duration<double, std::milli>(initializeEnd - initializeStart).count();

    DamageBatchWorkspace damageWorkspace;
    damageWorkspace.reserve(8, 64, 64);

    std::vector<double> editMs;
    std::vector<double> surfaceMs;
    std::vector<double> incrementalConnectivityMs;
    std::vector<double> referenceConnectivityMs;
    std::vector<double> nodesVisited;
    std::vector<double> bricksRecomputed;
    std::vector<double> topologyStableBricks;
    std::vector<double> metadataNodesVisited;
    std::uint64_t removed = 0;
    std::uint64_t generatedFaces = 0;
    std::uint64_t totalNodesVisited = 0;
    std::uint64_t totalBricksRecomputed = 0;
    std::uint64_t totalTopologyStableBricks = 0;
    std::uint64_t totalMetadataNodesVisited = 0;

    constexpr int frameCount = 180;
    for (int frame = 0; frame < frameCount; ++frame) {
        const float t = static_cast<float>(frame) / static_cast<float>(frameCount - 1);
        const Float3 center{
            4.0F + t * (static_cast<float>(bricksX * kBrickDim) - 8.0F),
            0.5F * static_cast<float>(bricksY * kBrickDim) + std::sin(t * 12.0F) * 8.0F,
            0.5F * static_cast<float>(bricksZ * kBrickDim),
        };

        const auto editStart = Clock::now();
        const std::array<SphereDamageCommand, 1> commands{{{center, 2.8F, static_cast<std::uint64_t>(frame)}}};
        const auto report = apply_damage_commands(world, commands, damageWorkspace);
        const auto editEnd = Clock::now();
        removed += report.removedVoxelCount;
        editMs.push_back(std::chrono::duration<double, std::milli>(editEnd - editStart).count());

        const auto surfaceStart = Clock::now();
        for (const AppliedBrickEdit& edit : report.edits) generatedFaces += extract_brick_surface(world, edit.key).size();
        const auto surfaceEnd = Clock::now();
        surfaceMs.push_back(std::chrono::duration<double, std::milli>(surfaceEnd - surfaceStart).count());

        const auto incrementalStart = Clock::now();
        const ConnectivityUpdateStats updateStats = connectivityCache.update_with_anchor_masks(world, report.edits, anchorMasks);
        const auto incrementalEnd = Clock::now();
        incrementalConnectivityMs.push_back(
            std::chrono::duration<double, std::milli>(incrementalEnd - incrementalStart).count());
        nodesVisited.push_back(static_cast<double>(updateStats.graphNodesVisited));
        bricksRecomputed.push_back(static_cast<double>(updateStats.bricksRecomputed));
        topologyStableBricks.push_back(static_cast<double>(updateStats.topologyStableBricks));
        metadataNodesVisited.push_back(static_cast<double>(updateStats.metadataNodesVisited));
        totalNodesVisited += updateStats.graphNodesVisited;
        totalBricksRecomputed += updateStats.bricksRecomputed;
        totalTopologyStableBricks += updateStats.topologyStableBricks;
        totalMetadataNodesVisited += updateStats.metadataNodesVisited;

        if (!connectivityCache.validate(world)) {
            std::cerr << "Incremental connectivity cache validation failed at frame " << frame << ".\n";
            return 2;
        }

        if (frame % 30 == 0) {
            const auto referenceStart = Clock::now();
            const auto reference = build_connectivity_snapshot(world, anchor);
            const auto referenceEnd = Clock::now();
            referenceConnectivityMs.push_back(
                std::chrono::duration<double, std::milli>(referenceEnd - referenceStart).count());
            if (!same_connectivity(connectivityCache.snapshot(), reference)) {
                std::cerr << "Incremental connectivity diverged from the full oracle at frame " << frame << ".\n";
                return 3;
            }
        }
    }

    GpuBrickMirror gpuMirror;
    const auto mirrorStart = Clock::now();
    gpuMirror.rebuild(world);
    const auto mirrorEnd = Clock::now();
    const double mirrorBuildMs = std::chrono::duration<double, std::milli>(mirrorEnd - mirrorStart).count();
    if (!gpuMirror.validate_against(world)) {
        std::cerr << "GPU brick mirror validation failed.\n";
        return 4;
    }

    std::array<std::uint64_t, 5> encodingCounts{};
    for (const auto& [key, brick] : world.bricks()) {
        (void)key;
        ++encodingCounts[static_cast<std::size_t>(brick.encoding())];
    }
    const BrickPoolStats poolStats = world.payload_pool_stats();
    const ConnectivityStorageStats connectivityStorage = connectivityCache.storage_stats();

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "{\n";
    std::cout << "  \"frames\": " << frameCount << ",\n";
    std::cout << "  \"remaining_voxels\": " << world.occupied_voxel_count() << ",\n";
    std::cout << "  \"removed_voxels\": " << removed << ",\n";
    std::cout << "  \"generated_bitwise_faces\": " << generatedFaces << ",\n";
    std::cout << "  \"brick_header_bytes\": " << sizeof(Brick) << ",\n";
    std::cout << "  \"logical_storage_bytes\": " << world.logical_storage_bytes() << ",\n";
    std::cout << "  \"flat_brick_map\": {\"entries\": " << world.brick_count()
              << ", \"entry_capacity\": " << world.bricks().capacity()
              << ", \"buckets\": " << world.bricks().bucket_count() << "},\n";
    std::cout << "  \"gpu_brick_mirror\": {\"build_ms\": " << mirrorBuildMs
              << ", \"slots\": " << gpuMirror.slot_count()
              << ", \"material_payload_bytes\": " << gpuMirror.payload_bytes() << "},\n";
    std::cout << "  \"pool_live_payload_bytes\": " << poolStats.livePayloadBytes << ",\n";
    std::cout << "  \"pool_reserved_bytes\": " << poolStats.reservedBytes << ",\n";
    std::cout << "  \"damage_workspace_capacities\": {\"commands\": " << damageWorkspace.command_capacity()
              << ", \"batches\": " << damageWorkspace.batch_capacity() << ", \"edits\": "
              << damageWorkspace.edit_capacity() << ", \"growth_events\": "
              << damageWorkspace.total_growth_events() << "},\n";
    std::cout << "  \"connectivity_initialize_ms\": " << initializeMs << ",\n";
    std::cout << "  \"connectivity_storage\": {\n";
    std::cout << "    \"ordinary_single_component_bytes\": " << ordinary_single_component_connectivity_bytes() << ",\n";
    std::cout << "    \"brick_count\": " << connectivityStorage.brickCount << ",\n";
    std::cout << "    \"local_components\": " << connectivityStorage.localComponentCount << ",\n";
    std::cout << "    \"graph_nodes\": " << connectivityStorage.graphNodeCount << ",\n";
    std::cout << "    \"graph_edges\": " << connectivityStorage.graphEdgeCount << ",\n";
    std::cout << "    \"live_bytes\": " << connectivityStorage.totalLiveBytes << ",\n";
    std::cout << "    \"capacity_bytes\": " << connectivityStorage.totalCapacityBytes << ",\n";
    std::cout << "    \"capacity_growth_events\": " << connectivityStorage.cumulativeCapacityGrowthEvents << "\n";
    std::cout << "  },\n";
    std::cout << "  \"edit_ms\": {\"p50\": " << percentile(editMs, 0.50) << ", \"p95\": "
              << percentile(editMs, 0.95) << ", \"p99\": " << percentile(editMs, 0.99) << "},\n";
    std::cout << "  \"surface_ms\": {\"p50\": " << percentile(surfaceMs, 0.50) << ", \"p95\": "
              << percentile(surfaceMs, 0.95) << ", \"p99\": " << percentile(surfaceMs, 0.99) << "},\n";
    std::cout << "  \"incremental_connectivity_ms\": {\"p50\": "
              << percentile(incrementalConnectivityMs, 0.50) << ", \"p95\": "
              << percentile(incrementalConnectivityMs, 0.95) << ", \"p99\": "
              << percentile(incrementalConnectivityMs, 0.99) << "},\n";
    std::cout << "  \"reference_connectivity_ms\": {\"p50\": "
              << percentile(referenceConnectivityMs, 0.50) << ", \"p95\": "
              << percentile(referenceConnectivityMs, 0.95) << ", \"p99\": "
              << percentile(referenceConnectivityMs, 0.99) << "},\n";
    std::cout << "  \"incremental_work\": {\n";
    std::cout << "    \"total_bricks_recomputed\": " << totalBricksRecomputed << ",\n";
    std::cout << "    \"bricks_recomputed_p99\": " << percentile(bricksRecomputed, 0.99) << ",\n";
    std::cout << "    \"total_topology_stable_bricks\": " << totalTopologyStableBricks << ",\n";
    std::cout << "    \"topology_stable_bricks_p99\": " << percentile(topologyStableBricks, 0.99) << ",\n";
    std::cout << "    \"total_graph_nodes_visited\": " << totalNodesVisited << ",\n";
    std::cout << "    \"graph_nodes_visited_p99\": " << percentile(nodesVisited, 0.99) << ",\n";
    std::cout << "    \"total_metadata_nodes_visited\": " << totalMetadataNodesVisited << ",\n";
    std::cout << "    \"metadata_nodes_visited_p99\": " << percentile(metadataNodesVisited, 0.99) << "\n";
    std::cout << "  },\n";
    std::cout << "  \"encodings\": {\n";
    std::cout << "    \"Empty\": " << encodingCounts[0] << ",\n";
    std::cout << "    \"UniformSolid\": " << encodingCounts[1] << ",\n";
    std::cout << "    \"MaskUniform\": " << encodingCounts[2] << ",\n";
    std::cout << "    \"LocalPalette4\": " << encodingCounts[3] << ",\n";
    std::cout << "    \"Palette8\": " << encodingCounts[4] << "\n";
    std::cout << "  },\n";
    std::cout << "  \"state_hash\": " << world.state_hash() << "\n";
    std::cout << "}\n";
    return world.validate() ? 0 : 1;
}
