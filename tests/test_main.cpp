#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/collision_proxy.hpp"
#include "dve/connectivity.hpp"
#include "dve/damage.hpp"
#include "dve/derived_jobs.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/fragment.hpp"
#include "dve/query.hpp"
#include "dve/surface.hpp"
#include "dve/work_debt.hpp"

namespace {

int failures = 0;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n";                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

void test_coordinate_mapping() {
    using namespace dve;
    CHECK(brick_key_from_voxel({-1, -1, -1}) == BrickKey{-1, -1, -1});
    CHECK(local_voxel_from_global({-1, -1, -1}) == Int3{7, 7, 7});
    CHECK(brick_key_from_voxel({8, 0, 0}) == BrickKey{1, 0, 0});
}

void test_adaptive_brick_encodings() {
    using namespace dve;
    BrickPayloadPools pools;
    Brick brick = Brick::uniform_solid(pools, 1);
    CHECK(brick.encoding() == BrickEncoding::UniformSolid);
    CHECK(brick.occupied_count() == 512);

    BrickMutation hole;
    hole.removeMask.set(0);
    brick.apply(hole);
    CHECK(brick.encoding() == BrickEncoding::MaskUniform);
    CHECK(brick.occupied_count() == 511);

    brick.set_voxel(1, 2);
    CHECK(brick.encoding() == BrickEncoding::LocalPalette4);
    CHECK(brick.material(1) == 2);

    for (std::uint16_t i = 2; i < 18; ++i) brick.set_voxel(i, static_cast<MaterialId>(i));
    CHECK(brick.encoding() == BrickEncoding::Palette8);
    CHECK(brick.validate());
}

void test_damage_does_not_create_empty_headers() {
    using namespace dve;
    VoxelObject object(9);
    object.fill_brick({0, 0, 0}, 1);
    const std::size_t before = object.brick_count();
    DamageBatchWorkspace workspace;
    const std::array<SphereDamageCommand, 1> commands{{{{1000.0F, 1000.0F, 1000.0F}, 4.0F, 1}}};
    const DamageApplyReportView report = apply_damage_commands(object, commands, workspace);
    CHECK(report.edits.empty());
    CHECK(report.removedVoxelCount == 0);
    CHECK(object.brick_count() == before);
}

void test_damage_and_determinism() {
    using namespace dve;
    VoxelObject a(10);
    VoxelObject b(10);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                a.fill_brick({x, y, z}, 1);
                b.fill_brick({x, y, z}, 1);
            }
        }
    }
    std::vector<SphereDamageCommand> commands{{{7.5F, 7.5F, 7.5F}, 3.0F, 2}, {{9.0F, 7.5F, 7.5F}, 2.0F, 1}};
    auto reversed = commands;
    std::reverse(reversed.begin(), reversed.end());
    const auto reportA = apply_damage_commands(a, commands);
    const auto reportB = apply_damage_commands(b, reversed);
    CHECK(reportA.removedVoxelCount > 0);
    CHECK(a.state_hash() == b.state_hash());
    CHECK(a.validate());
}

void test_surface_reference() {
    using namespace dve;
    VoxelObject object;
    object.set_voxel({0, 0, 0}, 1);
    auto quads = extract_brick_surface(object, {0, 0, 0});
    CHECK(quads.size() == 6);
    object.set_voxel({1, 0, 0}, 1);
    quads = extract_brick_surface(object, {0, 0, 0});
    CHECK(quads.size() == 10);
    CHECK(surface_hash(quads) != 0);
}


void test_flat_brick_map_and_deterministic_hash() {
    using namespace dve;
    VoxelObject forward(900);
    VoxelObject reverse(900);
    forward.reserve_bricks(256);
    reverse.reserve_bricks(256);
    std::vector<Int3> voxels;
    for (int z = -8; z < 16; z += 3) {
        for (int y = -8; y < 16; y += 3) {
            for (int x = -8; x < 16; x += 3) voxels.push_back({x, y, z});
        }
    }
    for (std::size_t i = 0; i < voxels.size(); ++i) {
        forward.set_voxel(voxels[i], static_cast<MaterialId>(1 + i % 7));
    }
    for (std::size_t i = voxels.size(); i-- > 0;) {
        reverse.set_voxel(voxels[i], static_cast<MaterialId>(1 + i % 7));
    }
    CHECK(forward.state_hash() == reverse.state_hash());
    CHECK(forward.validate());
    CHECK(reverse.validate());
    CHECK(forward.bricks().bucket_count() >= forward.brick_count());
}

void test_bitwise_surface_matches_reference() {
    using namespace dve;
    VoxelObject object(901);
    std::uint32_t state = 0x51FACEu;
    auto random = [&]() {
        state = state * 1664525U + 1013904223U;
        return state;
    };
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                for (int i = 0; i < 90; ++i) {
                    const Int3 local{
                        static_cast<int>(random() & 7U),
                        static_cast<int>((random() >> 3U) & 7U),
                        static_cast<int>((random() >> 6U) & 7U),
                    };
                    object.set_voxel(global_from_local({x, y, z}, local), static_cast<MaterialId>(1 + random() % 5U));
                }
            }
        }
    }
    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        const auto reference = extract_brick_surface_reference(object, key);
        const auto bitwise = extract_brick_surface_bitwise(object, key);
        CHECK(reference == bitwise);
        CHECK(surface_hash(reference) == surface_hash(bitwise));
    }
}

void test_quantized_damage_inputs() {
    using namespace dve;
    const auto a = quantize_damage_command({{1.0001F, -2.0001F, 3.4999F}, 2.2501F, 7});
    const auto b = quantize_damage_command({{1.0002F, -2.0002F, 3.5000F}, 2.2502F, 7});
    CHECK(a == b);

    VoxelObject first(902);
    VoxelObject second(902);
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                first.fill_brick({x, y, z}, 1);
                second.fill_brick({x, y, z}, 1);
            }
        }
    }
    std::array<SphereDamageCommand, 2> commands{{
        {{1.0001F, -2.0001F, 3.4999F}, 2.2501F, 7},
        {{-3.125F, 4.5F, 0.0F}, 1.75F, 8},
    }};
    auto reversed = commands;
    std::reverse(reversed.begin(), reversed.end());
    DamageBatchWorkspace firstWorkspace;
    DamageBatchWorkspace secondWorkspace;
    const auto firstReport = apply_damage_commands(first, commands, firstWorkspace);
    const auto secondReport = apply_damage_commands(second, reversed, secondWorkspace);
    CHECK(firstReport.removedVoxelCount == secondReport.removedVoxelCount);
    CHECK(first.state_hash() == second.state_hash());
}

void test_parallel_surface_jobs_are_deterministic() {
    using namespace dve;
    VoxelObject object(903);
    std::vector<BrickKey> keys;
    for (int z = 0; z < 3; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 8; ++x) {
                const BrickKey key{x, y, z};
                object.fill_brick(key, static_cast<MaterialId>(1 + (x + y + z) % 4));
                if (((x + 2 * y + z) % 3) == 0) object.set_voxel(global_from_local(key, {3, 3, 3}), kAirMaterial);
                keys.push_back(key);
            }
        }
    }
    std::reverse(keys.begin(), keys.end());
    keys.push_back(keys.front());
    JobSystem jobs(4);
    const auto first = extract_surfaces_parallel(object, keys, jobs);
    const auto second = extract_surfaces_parallel(object, keys, jobs);
    CHECK(first.size() == 96);
    CHECK(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].key == second[i].key);
        CHECK(first[i].generation == second[i].generation);
        CHECK(first[i].quads == second[i].quads);
        CHECK(first[i].quads == extract_brick_surface_reference(object, first[i].key));
    }
}


void test_gpu_brick_mirror_and_raycast_oracle() {
    using namespace dve;
    VoxelObject object(904);
    object.fill_brick({0, 0, 0}, 1); // UniformSolid
    object.fill_brick({1, 0, 0}, 2);
    object.set_voxel({8, 0, 0}, kAirMaterial); // MaskUniform
    object.fill_brick({0, 1, 0}, 3);
    object.set_voxel({1, 8, 0}, 4); // LocalPalette4
    object.fill_brick({1, 1, 0}, 5);
    for (std::uint16_t index = 0; index < 20; ++index) {
        object.set_voxel(global_from_local({1, 1, 0}, local_from_index_unchecked(index)),
                         static_cast<MaterialId>(1 + index));
    }

    GpuBrickMirror mirror;
    mirror.rebuild(object);
    CHECK(mirror.slot_count() == object.brick_count());
    CHECK(mirror.validate_against(object));

    std::uint32_t state = 0xBADC0DEu;
    auto random = [&]() {
        state = state * 1664525U + 1013904223U;
        return state;
    };
    for (int i = 0; i < 3000; ++i) {
        const Float3 origin{
            static_cast<float>(static_cast<int>(random() % 48U) - 8) + 0.25F,
            static_cast<float>(static_cast<int>(random() % 40U) - 8) + 0.5F,
            static_cast<float>(static_cast<int>(random() % 24U) - 8) + 0.75F,
        };
        const Float3 direction{
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
        };
        const auto cpu = raycast_voxels(object, origin, direction, 80.0F);
        const auto gpuReference = raycast_gpu_brick_mirror(mirror, origin, direction, 80.0F);
        CHECK(cpu.has_value() == gpuReference.has_value());
        if (cpu && gpuReference) {
            CHECK(cpu->voxel == gpuReference->voxel);
            CHECK(cpu->normal == gpuReference->normal);
            CHECK(cpu->material == gpuReference->material);
            CHECK(std::abs(cpu->distance - gpuReference->distance) < 1.0e-5F);
        }
    }

    BrickMutation firstMutation;
    firstMutation.removeMask.set(voxel_index_unchecked({7, 0, 0}));
    const AppliedBrickEdit stale = object.apply({0, 0, 0}, firstMutation);
    BrickMutation secondMutation;
    secondMutation.removeMask.set(voxel_index_unchecked({6, 0, 0}));
    const AppliedBrickEdit current = object.apply({0, 0, 0}, secondMutation);
    const std::array<AppliedBrickEdit, 2> edits{stale, current};
    const GpuMirrorUpdateStats stats = mirror.update(object, edits);
    CHECK(stats.submitted == 2);
    CHECK(stats.staleDropped == 1);
    CHECK(stats.published >= 2); // edited brick plus occupied +X neighbor halo
    CHECK(mirror.validate_against(object));
}

void test_queries() {
    using namespace dve;
    VoxelObject object;
    object.set_voxel({3, 0, 0}, 7);
    const auto hit = raycast_voxels(object, {0.5F, 0.5F, 0.5F}, {1.0F, 0.0F, 0.0F}, 10.0F);
    CHECK(hit.has_value());
    CHECK(hit->voxel == Int3{3, 0, 0});
    CHECK(hit->normal == Int3{-1, 0, 0});
    CHECK(hit->material == 7);
    CHECK(overlaps_voxels(object, {2.9F, 0.1F, 0.1F}, {3.2F, 0.9F, 0.9F}));
    CHECK(!overlaps_voxels(object, {0.0F, 2.0F, 0.0F}, {1.0F, 3.0F, 1.0F}));
}

void test_connectivity_and_split_transaction() {
    using namespace dve;
    VoxelObject object(42);

    // Two 2x2x2 masses joined by one voxel. The left mass touches z=0 and is anchored.
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) object.set_voxel({x, y, z}, 1);
            for (int x = 4; x < 6; ++x) object.set_voxel({x, y, z + 1}, 2);
        }
    }
    object.set_voxel({2, 0, 1}, 1);
    object.set_voxel({3, 0, 1}, 2);

    auto snapshot = build_connectivity_snapshot(object, [](Int3 voxel) { return voxel.z == 0; });
    CHECK(snapshot.components.size() == 1);
    CHECK(snapshot.components[0].anchored);

    object.set_voxel({2, 0, 1}, kAirMaterial);
    snapshot = build_connectivity_snapshot(object, [](Int3 voxel) { return voxel.z == 0; });
    CHECK(snapshot.components.size() == 2);

    std::size_t detachedIndex = 0;
    for (std::size_t i = 0; i < snapshot.components.size(); ++i) {
        if (!snapshot.components[i].anchored) detachedIndex = i;
    }
    const auto plan = build_split_plan(object, snapshot, detachedIndex);
    CHECK(plan.has_value());
    CHECK(plan->voxelCount > 0);
    const std::uint64_t before = object.occupied_voxel_count();
    auto detached = commit_split_plan(object, *plan, 99);
    CHECK(detached.has_value());
    CHECK(detached->occupied_voxel_count() == plan->voxelCount);
    CHECK(object.occupied_voxel_count() + detached->occupied_voxel_count() == before);
    CHECK(object.validate());
    CHECK(detached->validate());
}


void test_box_collision_proxy() {
    using namespace dve;
    VoxelObject object;
    for (int z = 0; z < 3; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 5; ++x) object.set_voxel({x, y, z}, 1);
        }
    }
    object.set_voxel({4, 2, 0}, 1); // forces a second box
    const auto boxes = build_object_box_proxy(object);
    CHECK(!boxes.empty());
    CHECK(boxes.size() <= 3);
    CHECK(validate_box_proxy(object, boxes));
}

void test_work_debt_governor() {
    using namespace dve;
    WorkDebtGovernor governor(64);
    WorkBudget budget;
    for (int i = 0; i < 20; ++i) {
        budget = governor.update({13.0, 9.0, 500, 200, 16ULL * 1024ULL * 1024ULL, 220});
    }
    CHECK(budget.pressure == PressureLevel::Emergency || budget.pressure == PressureLevel::Recovery);
    CHECK(!budget.allowAsyncQualityUpgrades);
    const auto pressured = budget.pressure;
    for (int i = 0; i < 300; ++i) budget = governor.update({4.0, 4.0, 0, 0, 0, 10});
    CHECK(static_cast<int>(budget.pressure) < static_cast<int>(pressured));
}

void test_stale_split_rejected() {
    using namespace dve;
    VoxelObject object(7);
    object.set_voxel({0, 0, 0}, 1);
    object.set_voxel({2, 0, 0}, 1);
    auto snapshot = build_connectivity_snapshot(object);
    CHECK(snapshot.components.size() == 2);
    auto plan = build_split_plan(object, snapshot, 0);
    CHECK(plan.has_value());
    object.set_voxel({0, 1, 0}, 1); // changes a source generation
    auto detached = commit_split_plan(object, *plan, 8);
    CHECK(!detached.has_value());
}


bool same_connectivity(const dve::ConnectivitySnapshot& a, const dve::ConnectivitySnapshot& b) {
    if (a.brickData.size() != b.brickData.size() || a.components.size() != b.components.size()) return false;
    auto ita = a.brickData.begin();
    auto itb = b.brickData.begin();
    for (; ita != a.brickData.end(); ++ita, ++itb) {
        if (ita->first != itb->first) return false;
        const auto& da = ita->second;
        const auto& db = itb->second;
        if (da.generation != db.generation || da.components != db.components) {
            return false;
        }
        for (std::size_t i = 0; i < da.components.size(); ++i) {
            const auto& ca = da.components[i];
            const auto& cb = db.components[i];
            if (ca.voxelCount != cb.voxelCount || ca.faceMask != cb.faceMask || ca.minLocal != cb.minLocal ||
                ca.maxLocal != cb.maxLocal || ca.anchored != cb.anchored) {
                return false;
            }
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

void test_payload_pools_and_direct_mutation() {
    using namespace dve;
    CHECK(sizeof(Brick) <= 32);
    BrickPayloadPools pools;
    {
        Brick brick = Brick::uniform_solid(pools, 3);
        CHECK(pools.stats().livePayloadBytes == 0);
        BrickMutation holes;
        for (std::uint16_t i = 0; i < 64; ++i) holes.removeMask.set(i);
        brick.apply(holes);
        CHECK(brick.encoding() == BrickEncoding::MaskUniform);
        CHECK(pools.stats().liveMaskUniform == 1);
        const auto reserved = pools.stats().reservedBytes;
        for (std::uint16_t i = 64; i < 128; ++i) brick.set_voxel(i, 4);
        CHECK(brick.encoding() == BrickEncoding::LocalPalette4);
        CHECK(pools.stats().reservedBytes >= reserved);
        CHECK(brick.validate());
    }
    CHECK(pools.stats().livePayloadBytes == 0);
}

void test_incremental_connectivity_cache() {
    using namespace dve;
    VoxelObject object(123);
    for (int z = 0; z < 3; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 6; ++x) object.fill_brick({x, y, z}, 1);
        }
    }
    const AnchorPredicate anchor = [](Int3 voxel) { return voxel.z == 0; };
    IncrementalConnectivityCache cache;
    cache.initialize(object, anchor);
    CHECK(cache.validate(object));
    CHECK(same_connectivity(cache.snapshot(), build_connectivity_snapshot(object, anchor)));

    std::size_t totalRecomputed = 0;
    for (int frame = 0; frame < 24; ++frame) {
        const Float3 center{8.0F + static_cast<float>(frame) * 1.25F, 15.0F, 10.0F};
        const auto report = apply_damage_commands(object, {{{center}, 2.4F, static_cast<std::uint64_t>(frame)}});
        const auto stats = cache.update(object, report.edits, anchor);
        totalRecomputed += stats.bricksRecomputed;
        CHECK(cache.validate(object));
        CHECK(same_connectivity(cache.snapshot(), build_connectivity_snapshot(object, anchor)));
    }
    CHECK(totalRecomputed < object.brick_count());

    // Explicit split across brick boundaries: remove a two-voxel bridge and verify the cache
    // produces the same detached component as the full oracle.
    VoxelObject bridge(124);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 8; ++x) bridge.set_voxel({x, y, z}, 1);
            for (int x = 10; x < 18; ++x) bridge.set_voxel({x, y, z + 1}, 2);
        }
    }
    bridge.set_voxel({8, 0, 1}, 1);
    bridge.set_voxel({9, 0, 1}, 2);
    IncrementalConnectivityCache bridgeCache;
    bridgeCache.initialize(bridge, anchor);
    BrickMutation cut;
    cut.removeMask.set(voxel_index(local_voxel_from_global({8, 0, 1})));
    const AppliedBrickEdit edit = bridge.apply(brick_key_from_voxel({8, 0, 1}), cut);
    const std::array<AppliedBrickEdit, 1> edits{edit};
    const auto bridgeStats = bridgeCache.update(bridge, edits, anchor);
    CHECK(bridgeStats.bricksRecomputed == 1);
    const auto incremental = bridgeCache.snapshot();
    const auto reference = build_connectivity_snapshot(bridge, anchor);
    CHECK(same_connectivity(incremental, reference));
    CHECK(incremental.components.size() == 2);
}


void test_brick_mutation_fuzz() {
    using namespace dve;
    BrickPayloadPools pools;
    Brick brick(pools);
    std::array<MaterialId, kBrickVoxelCount> reference{};
    std::uint32_t state = 0xC001D00DU;
    auto random = [&]() {
        state = state * 1664525U + 1013904223U;
        return state;
    };

    for (int step = 0; step < 1500; ++step) {
        BrickMutation mutation;
        const int removes = static_cast<int>(random() % 12U);
        for (int i = 0; i < removes; ++i) {
            const auto index = static_cast<std::uint16_t>(random() % kBrickVoxelCount);
            mutation.removeMask.set(index);
            reference[index] = kAirMaterial;
        }
        const int writes = static_cast<int>(random() % 6U);
        for (int i = 0; i < writes; ++i) {
            const auto index = static_cast<std::uint16_t>(random() % kBrickVoxelCount);
            const auto material = static_cast<MaterialId>(random() % 32U);
            mutation.writes.push_back({index, material});
            reference[index] = material;
        }
        brick.apply(mutation);
        CHECK(brick.validate());
        for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
            CHECK(brick.material(index) == reference[index]);
        }
    }
    brick.compact_encoding();
    CHECK(brick.validate());
}

void test_incremental_randomized_oracle() {
    using namespace dve;
    VoxelObject object(501);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 4; ++x) object.fill_brick({x, y, z}, 1);
        }
    }
    const AnchorPredicate anchor = [](Int3 voxel) { return voxel.z == 0; };
    IncrementalConnectivityCache cache;
    cache.initialize(object, anchor);

    std::uint32_t state = 0xA11CE55U;
    auto random = [&]() {
        state = state * 1103515245U + 12345U;
        return state;
    };
    for (int step = 0; step < 100; ++step) {
        const Int3 voxel{
            static_cast<int>(random() % 32U),
            static_cast<int>(random() % 24U),
            static_cast<int>(random() % 16U),
        };
        const BrickKey key = brick_key_from_voxel(voxel);
        BrickMutation mutation;
        mutation.writes.push_back({
            voxel_index(local_voxel_from_global(voxel)),
            (random() % 5U == 0U) ? static_cast<MaterialId>(1 + random() % 5U) : kAirMaterial,
        });
        const AppliedBrickEdit edit = object.apply(key, mutation);
        const std::array<AppliedBrickEdit, 1> edits{edit};
        const auto stats = cache.update(object, edits, anchor);
        (void)stats;
        CHECK(cache.validate(object));
        CHECK(same_connectivity(cache.snapshot(), build_connectivity_snapshot(object, anchor)));
    }
}



void test_transformed_queries_and_player_safety() {
    using namespace dve;
    constexpr float pi = 3.14159265358979323846F;
    VoxelObject object(907);
    object.set_voxel({3, 0, 0}, 7);
    const Quaternion quarterTurn{0.0F, 0.0F, std::sin(pi * 0.25F), std::cos(pi * 0.25F)};
    const RigidTransform transform = make_rigid_transform({10.0F, 0.0F, 0.0F}, quarterTurn);
    const auto hit = raycast_voxels_transformed(
        object, transform, {9.5F, 0.0F, 0.5F}, {0.0F, 1.0F, 0.0F}, 10.0F);
    CHECK(hit.has_value());
    CHECK(hit->objectHit.voxel == Int3{3, 0, 0});
    CHECK(std::abs(hit->worldPosition.y - 3.0F) < 1.0e-4F);
    CHECK(hit->worldNormal.y < -0.99F);

    VoxelObject wall(908);
    wall.set_voxel({3, 0, 0}, 4);
    const Capsule capsule{{0.5F, 0.5F, 0.2F}, {0.5F, 0.5F, 0.8F}, 0.3F};
    const auto sweep = sweep_capsule_conservative(wall, {}, capsule, {4.0F, 0.0F, 0.0F});
    CHECK(sweep.has_value());
    CHECK(sweep->time >= 0.0F && sweep->time <= 1.0F);
    CHECK(sweep->worldNormal.x < -0.99F);
    CHECK(sweep->voxel == Int3{3, 0, 0});

    VoxelObject floor(909);
    floor.set_voxel({0, 0, 0}, 1);
    PlayerCollisionState player{
        {{0.5F, 0.5F, 1.45F}, {0.5F, 0.5F, 2.45F}, 0.45F},
        {0.0F, 0.0F, 0.0F},
        true,
    };
    const PlayerEditResolution before = resolve_player_after_voxel_edit(floor, {}, player);
    CHECK(before.grounded);
    CHECK(!before.floorRemoved);
    floor.set_voxel({0, 0, 0}, kAirMaterial);
    player.grounded = true;
    const PlayerEditResolution after = resolve_player_after_voxel_edit(floor, {}, player);
    CHECK(!after.grounded);
    CHECK(after.floorRemoved);

    VoxelObject block(910);
    block.set_voxel({0, 0, 0}, 1);
    Capsule embedded{{0.5F, 0.5F, 0.2F}, {0.5F, 0.5F, 0.8F}, 0.35F};
    CHECK(capsule_overlaps_voxels(block, {}, embedded));
    const Float3 correction = depenetrate_capsule_conservative(block, {}, embedded, 12, 0.002F);
    embedded.pointA = add(embedded.pointA, correction);
    embedded.pointB = add(embedded.pointB, correction);
    CHECK(!capsule_overlaps_voxels(block, {}, embedded));
}

void test_conservative_capsule_sweep_randomized_oracle() {
    using namespace dve;
    VoxelObject object(911);
    std::uint32_t state = 0x9E3779B9U;
    auto random = [&]() {
        state = state * 1664525U + 1013904223U;
        return state;
    };
    for (int i = 0; i < 80; ++i) {
        object.set_voxel({
            static_cast<int>(random() % 12U) - 2,
            static_cast<int>(random() % 12U) - 2,
            static_cast<int>(random() % 8U) - 1,
        }, 1);
    }
    constexpr float pi = 3.14159265358979323846F;
    const RigidTransform transform = make_rigid_transform(
        {2.0F, -1.5F, 0.75F},
        {0.0F, 0.0F, std::sin(pi / 12.0F), std::cos(pi / 12.0F)});

    for (int trial = 0; trial < 350; ++trial) {
        const Float3 base{
            static_cast<float>(static_cast<int>(random() % 1800U) - 600) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 1800U) - 600) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 900U) - 200) / 100.0F,
        };
        const float height = 0.4F + static_cast<float>(random() % 180U) / 100.0F;
        const Capsule capsule{base, add(base, {0.0F, 0.0F, height}), 0.15F + static_cast<float>(random() % 40U) / 100.0F};
        Float3 displacement{
            static_cast<float>(static_cast<int>(random() % 1000U) - 500) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 1000U) - 500) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 600U) - 300) / 100.0F,
        };
        if (length_squared(displacement) < 0.01F) displacement.x = 1.0F;

        bool denseOracleHit = false;
        for (int sample = 0; sample <= 320; ++sample) {
            const float t = static_cast<float>(sample) / 320.0F;
            Capsule moved = capsule;
            moved.pointA = add(moved.pointA, multiply(displacement, t));
            moved.pointB = add(moved.pointB, multiply(displacement, t));
            if (capsule_overlaps_voxels(object, transform, moved)) {
                denseOracleHit = true;
                break;
            }
        }
        const auto conservative = sweep_capsule_conservative(object, transform, capsule, displacement);
        CHECK(!denseOracleHit || conservative.has_value());
    }

    const MovingRigidTransform moving{
        make_rigid_transform({0.0F, 0.0F, 0.0F}, {}),
        make_rigid_transform({1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, std::sin(pi / 8.0F), std::cos(pi / 8.0F)}),
    };
    const Capsule probe{{-1.0F, 0.5F, 0.5F}, {-1.0F, 0.5F, 1.5F}, 0.3F};
    (void)sweep_capsule_against_moving_body(object, moving, 0.5F, probe, {4.0F, 0.0F, 0.0F});
}

void test_compact_connectivity_memory_and_csr() {
    using namespace dve;
    CHECK(ordinary_single_component_connectivity_bytes() < 160);

    VoxelObject object(905);
    object.reserve_bricks(256);
    for (int z = 0; z < 4; ++z) {
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 8; ++x) object.fill_brick({x, y, z}, 1);
        }
    }
    const AnchorMaskProvider anchors = [](BrickKey key) {
        Bitset512 mask;
        if (key.z == 0) mask.words[0] = ~std::uint64_t{0};
        return mask;
    };
    IncrementalConnectivityCache cache;
    cache.reserve(256, 256, 1024);
    cache.initialize_with_anchor_masks(object, anchors);
    CHECK(cache.validate(object));
    CHECK(cache.graph_node_count() == object.brick_count());
    CHECK(cache.graph_edge_count() > 0);

    const ConnectivityStorageStats memory = cache.storage_stats();
    CHECK(memory.localComponentCount == object.brick_count());
    const std::size_t perBrickCompact =
        (memory.brickHeaderBytes + memory.localComponentLiveBytes) / memory.brickCount;
    CHECK(perBrickCompact <= 160);

    DamageBatchWorkspace workspace;
    workspace.reserve(4, 64, 64);
    const std::array<SphereDamageCommand, 1> commands{{{{12.5F, 12.5F, 12.5F}, 1.2F, 1}}};
    const DamageApplyReportView report = apply_damage_commands(object, commands, workspace);
    CHECK(workspace.last_growth_events() == 0);
    const ConnectivityUpdateStats update = cache.update_with_anchor_masks(object, report.edits, anchors);
    CHECK(update.graphCapacityGrowthEvents == 0);
    CHECK(update.scratchCapacityGrowthEvents == 0);
    CHECK(cache.validate(object));
    CHECK(same_connectivity(cache.snapshot(), build_connectivity_snapshot_with_anchor_masks(object, anchors)));
}


void test_fragment_mass_properties_and_solver_package() {
    using namespace dve;

    VoxelObject single(10001);
    single.set_voxel({0, 0, 0}, 1);
    const FragmentMassProperties one = compute_fragment_mass_properties(single);
    CHECK(one.voxelCount == 1);
    CHECK(one.massUnits == 1);
    CHECK(std::abs(one.centerOfMass.x - 0.5F) < 1.0e-6F);
    CHECK(std::abs(one.centerOfMass.y - 0.5F) < 1.0e-6F);
    CHECK(std::abs(one.centerOfMass.z - 0.5F) < 1.0e-6F);
    CHECK(std::abs(one.inertiaAboutCenter.xx - 1.0 / 6.0) < 1.0e-12);
    CHECK(std::abs(one.inertiaAboutCenter.yy - 1.0 / 6.0) < 1.0e-12);
    CHECK(std::abs(one.inertiaAboutCenter.zz - 1.0 / 6.0) < 1.0e-12);

    VoxelObject pair(10002);
    pair.set_voxel({0, 0, 0}, 1);
    pair.set_voxel({1, 0, 0}, 1);
    const FragmentMassProperties two = compute_fragment_mass_properties(pair);
    CHECK(two.voxelCount == 2);
    CHECK(two.massUnits == 2);
    CHECK(std::abs(two.centerOfMass.x - 1.0F) < 1.0e-6F);
    CHECK(std::abs(two.inertiaAboutCenter.xx - 1.0 / 3.0) < 1.0e-12);
    CHECK(std::abs(two.inertiaAboutCenter.yy - 5.0 / 6.0) < 1.0e-12);
    CHECK(std::abs(two.inertiaAboutCenter.zz - 5.0 / 6.0) < 1.0e-12);

    MaterialMassTable masses;
    masses.set_density_units(1, 1);
    masses.set_density_units(2, 3);
    VoxelObject weighted(10003);
    weighted.set_voxel({0, 0, 0}, 1);
    weighted.set_voxel({1, 0, 0}, 2);
    const FragmentMassProperties weightedProperties = compute_fragment_mass_properties(weighted, masses);
    CHECK(weightedProperties.massUnits == 4);
    CHECK(std::abs(weightedProperties.centerOfMass.x - 1.25F) < 1.0e-6F);

    VoxelObject acrossBricks(10004);
    for (int x = 0; x < 16; ++x) acrossBricks.set_voxel({x, 0, 0}, 1);
    const auto unmerged = build_object_box_proxy(acrossBricks);
    const auto merged = build_merged_object_box_proxy(acrossBricks);
    CHECK(unmerged.size() == 2);
    CHECK(merged.size() == 1);
    CHECK(merged.front() == VoxelBox{{0, 0, 0}, {16, 1, 1}});
    CHECK(validate_box_proxy(acrossBricks, merged));

    constexpr float pi = 3.14159265358979323846F;
    const RigidTransform world = make_rigid_transform(
        {10.0F, -2.0F, 3.0F},
        {0.0F, 0.0F, std::sin(pi / 4.0F), std::cos(pi / 4.0F)});
    const FragmentSolverPackage package = build_fragment_solver_package(acrossBricks, world, {}, 1);
    CHECK(package.unmergedBoxCount == 2);
    CHECK(package.boxes.size() == 1);
    CHECK(!package.proxyOverBudget);
    CHECK(validate_solver_package(acrossBricks, package));
    const Float3 expectedBodyPosition = transform_point(world, package.mass.centerOfMass);
    CHECK(length(subtract(package.bodyTransform.position, expectedBodyPosition)) < 1.0e-5F);

    const FragmentSolverPackage noCap = build_fragment_solver_package(acrossBricks, world, {}, 0);
    CHECK(!noCap.proxyOverBudget); // zero disables the cap

    VoxelObject separated(10005);
    separated.set_voxel({0, 0, 0}, 1);
    separated.set_voxel({3, 0, 0}, 1);
    const FragmentSolverPackage capped = build_fragment_solver_package(separated, {}, {}, 1);
    CHECK(capped.boxes.size() == 2);
    CHECK(capped.proxyOverBudget);

    VoxelObject oversizedExtent(10006);
    oversizedExtent.set_voxel({0, 0, 0}, 1);
    oversizedExtent.set_voxel({kMaximumDynamicFragmentExtent, 0, 0}, 1);
    const FragmentMassProperties rejected = compute_fragment_mass_properties(oversizedExtent);
    CHECK(!rejected.exactWithinLimits);
}

void test_damage_workspace_growth_telemetry() {
    using namespace dve;
    VoxelObject object(906);
    object.reserve_bricks(64);
    object.reserve_payloads(64, 16, 16);
    for (int z = 0; z < 3; ++z) {
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) object.fill_brick({x, y, z}, 2);
        }
    }
    DamageBatchWorkspace workspace;
    workspace.reserve(8, 128, 128);
    const std::array<SphereDamageCommand, 2> commands{{
        {{8.0F, 8.0F, 8.0F}, 2.0F, 1},
        {{12.0F, 12.0F, 12.0F}, 1.5F, 2},
    }};
    const BrickPoolStats beforePools = object.payload_pool_stats();
    const std::size_t beforeBrickCapacity = object.bricks().capacity();
    const std::size_t beforeBuckets = object.bricks().bucket_count();
    (void)apply_damage_commands(object, commands, workspace);
    CHECK(workspace.last_growth_events() == 0);
    CHECK(object.payload_pool_stats().reservedBytes == beforePools.reservedBytes);
    CHECK(object.bricks().capacity() == beforeBrickCapacity);
    CHECK(object.bricks().bucket_count() == beforeBuckets);
    const std::size_t total = workspace.total_growth_events();
    (void)apply_damage_commands(object, commands, workspace);
    CHECK(workspace.last_growth_events() == 0);
    CHECK(workspace.total_growth_events() == total);
    CHECK(object.payload_pool_stats().reservedBytes == beforePools.reservedBytes);
}

void test_packed_brickmap_and_hdda_oracle() {
    using namespace dve;
    VoxelObject object(11001);
    object.reserve_bricks(128);
    for (int z = -2; z <= 2; ++z) {
        for (int y = -1; y <= 2; ++y) {
            for (int x = -3; x <= 3; ++x) {
                if (((x * 3 + y * 5 + z * 7) & 3) == 0) continue;
                object.fill_brick({x, y, z}, static_cast<MaterialId>(1 + ((x - y + z) & 7)));
            }
        }
    }
    // Force every material encoding into the packed scene.
    object.set_voxel({-24, -8, -16}, kAirMaterial);
    object.set_voxel({0, 0, 0}, 9);
    for (int i = 0; i < 20; ++i) object.set_voxel({8 + i % 8, 8 + (i / 8), 8}, static_cast<MaterialId>(10 + i));

    PackedBrickmapScene packed;
    packed.reserve(object.brick_count() + 16, object.payload_bytes() + 4096);
    packed.rebuild(object);
    CHECK(packed.validate_against(object));
    CHECK(packed.records().size() == object.brick_count());
    CHECK(packed.bounds().valid);
    CHECK(packed.readback_hash() != 0);

    GpuBrickMirror mirror;
    mirror.rebuild(object);
    std::uint32_t randomState = 0xC0FFEE12U;
    auto random = [&]() {
        randomState = randomState * 1664525U + 1013904223U;
        return randomState;
    };
    for (int rayIndex = 0; rayIndex < 4000; ++rayIndex) {
        const Float3 origin{
            -40.0F + static_cast<float>(random() % 8000U) / 100.0F,
            -24.0F + static_cast<float>(random() % 6000U) / 100.0F,
            -32.0F + static_cast<float>(random() % 8000U) / 100.0F,
        };
        Float3 direction{
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
            static_cast<float>(static_cast<int>(random() % 2001U) - 1000),
        };
        if (length_squared(direction) < 1.0F) direction = {1.0F, 0.25F, -0.5F};
        const auto authority = raycast_voxels(object, origin, direction, 180.0F);
        const auto mirrorHit = raycast_gpu_brick_mirror(mirror, origin, direction, 180.0F);
        BrickmapRayTraceStats traceStats;
        const auto packedHit = raycast_packed_brickmap_hdda(packed, origin, direction, 180.0F, &traceStats);
        CHECK(authority.has_value() == mirrorHit.has_value());
        CHECK(authority.has_value() == packedHit.has_value());
        if (authority && packedHit) {
            CHECK(authority->voxel == packedHit->voxel);
            CHECK(authority->normal == packedHit->normal);
            CHECK(authority->material == packedHit->material);
            CHECK(std::abs(authority->distance - packedHit->distance) < 2.0e-4F);
        }
    }

    const std::uint64_t oldHash = packed.readback_hash();
    DamageBatchWorkspace workspace;
    const std::array<SphereDamageCommand, 1> commands{{{{2.0F, 2.0F, 2.0F}, 3.5F, 1}}};
    const DamageApplyReportView edits = apply_damage_commands(object, commands, workspace);
    const PackedBrickmapUpdateStats update = packed.update(object, edits.edits);
    CHECK(update.published > 0);
    CHECK(packed.validate_against(object));
    CHECK(packed.readback_hash() != oldHash);

    const BrickKey editedKey = edits.edits.front().key;
    GpuBrickUpload stale = build_gpu_brick_upload(object, editedKey);
    CHECK(stale.generation > 0U);
    --stale.generation;
    stale.uniformMaterial = 255U;
    const PackedBrickmapUpdateStats staleStats = packed.publish_uploads(std::span<const GpuBrickUpload>(&stale, 1));
    CHECK(staleStats.staleDropped == 1);
    CHECK(packed.validate_against(object));
}

void test_fence_upload_ring() {
    using namespace dve;
    FenceUploadRing ring(1024);
    const auto a = ring.allocate(300, 64, 1, 0);
    const auto b = ring.allocate(500, 64, 2, 0);
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(a->offset % 64 == 0);
    CHECK(b->offset % 64 == 0);
    CHECK(ring.in_flight_bytes() == 800);
    CHECK(!ring.allocate(400, 64, 3, 0).has_value());
    ring.retire(1);
    const auto c = ring.allocate(256, 64, 3, 1);
    CHECK(c.has_value());
    CHECK(c->offset == 0);
    auto bytes = ring.mapped_span(*c);
    CHECK(bytes.size() == 256);
    std::fill(bytes.begin(), bytes.end(), std::byte{0x5A});
    ring.retire(3);
    CHECK(ring.allocation_count() == 0);
}


void test_rigid_body_validation_and_split_velocity() {
    using namespace dve;

    RigidBodyCreateDesc valid;
    valid.transform = {{1.0F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F, 2.0F}}; // valid but not normalized
    valid.massKilograms = 2.0;
    valid.inertiaKilogramMetersSquared = {1.0, 2.0, 3.0, 0.1, 0.05, 0.02};
    valid.boxes = {SolverBox{{}, {0.5F, 0.5F, 0.5F}}};
    const RigidBodyValidationResult validation = validate_rigid_body_desc(valid);
    CHECK(validation);
    CHECK(std::fabs(validation.normalizedTransform.rotation.w - 1.0F) < 1.0e-6F);

    RigidBodyCreateDesc invalid = valid;
    invalid.boxes.front().halfExtents.x = 0.0F;
    CHECK(validate_rigid_body_desc(invalid).error == RigidBodyValidationError::InvalidCollisionBox);

    invalid = valid;
    invalid.transform.rotation = {0.0F, 0.0F, 0.0F, 0.0F};
    CHECK(validate_rigid_body_desc(invalid).error == RigidBodyValidationError::InvalidTransform);

    invalid = valid;
    invalid.inertiaKilogramMetersSquared = {1.0, 1.0, 0.0, 0.0, 0.0, 0.0};
    CHECK(inertia_is_positive_semidefinite(invalid.inertiaKilogramMetersSquared));
    CHECK(!inertia_is_positive_definite(invalid.inertiaKilogramMetersSquared));
    CHECK(validate_rigid_body_desc(invalid).error ==
        RigidBodyValidationError::SingularInertiaAfterFloatConversion);

    const Float3 inherited = inherited_child_center_of_mass_velocity(
        {1.0F, 2.0F, 3.0F},
        {0.0F, 0.0F, 2.0F},
        {10.0F, 0.0F, 0.0F},
        {11.0F, 0.0F, 0.0F});
    CHECK(std::fabs(inherited.x - 1.0F) < 1.0e-6F);
    CHECK(std::fabs(inherited.y - 4.0F) < 1.0e-6F);
    CHECK(std::fabs(inherited.z - 3.0F) < 1.0e-6F);

    ReferenceRigidBodyWorld world;
    const RigidBodyHandle handle = world.create_body(valid);
    CHECK(handle != kInvalidRigidBodyHandle);
    RigidBodyState sleepingState;
    sleepingState.previousTransform = validation.normalizedTransform;
    sleepingState.currentTransform = validation.normalizedTransform;
    sleepingState.sleeping = true;
    CHECK(world.set_state(handle, sleepingState));
    CHECK(world.body_count() == 1U);
    CHECK(world.dynamic_body_count() == 1U);
    CHECK(world.awake_body_count() == 0U);
    CHECK(world.sleeping_body_count() == 1U);
    CHECK(world.active_body_count() == 0U);
    CHECK(world.destroy_body(handle));
}

void test_rigid_body_adapter() {
    using namespace dve;
    VoxelObject object(11002);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 12; ++x) object.set_voxel({x, y, z}, 1);
        }
    }
    const FragmentSolverPackage package = build_fragment_solver_package(
        object, make_rigid_transform({2.0F, 5.0F, -1.0F}, {}));
    const auto desc = make_rigid_body_desc(package, {0.002, 0.25}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    CHECK(desc.has_value());
    CHECK(desc->massKilograms > 0.0);
    CHECK(inertia_is_positive_semidefinite(desc->inertiaKilogramMetersSquared));
    CHECK(desc->boxes.size() == package.boxes.size());
    CHECK(length(subtract(desc->transform.position, multiply(package.bodyTransform.position, 0.25F))) < 1.0e-6F);

    FragmentSolverPackage movingChild = package;
    movingChild.bodyTransform.position = {44.0F, 0.0F, 0.0F}; // 11 meters at 0.25 m/voxel
    RigidBodyState parentState;
    parentState.previousTransform = make_rigid_transform({10.0F, 0.0F, 0.0F}, {});
    parentState.currentTransform = parentState.previousTransform;
    parentState.linearVelocity = {1.0F, 2.0F, 3.0F};
    parentState.angularVelocity = {0.0F, 0.0F, 2.0F};
    const auto movingDesc = make_moving_split_rigid_body_desc(movingChild, {0.002, 0.25}, parentState);
    CHECK(movingDesc.has_value());
    CHECK(length(subtract(movingDesc->transform.position, Float3{11.0F, 0.0F, 0.0F})) < 1.0e-6F);
    CHECK(length(subtract(movingDesc->linearVelocity, Float3{1.0F, 4.0F, 3.0F})) < 1.0e-6F);

    ReferenceRigidBodyWorld world;
    const RigidBodyHandle handle = world.create_body(*desc);
    CHECK(handle != kInvalidRigidBodyHandle);
    CHECK(world.active_body_count() == 1);
    const auto velocityBeforePointLoad = world.state(handle);
    CHECK(world.apply_force_at_point(handle, {0.0F, 12.0F, 0.0F}, {3.0F, 6.0F, -1.0F}));
    const auto velocityAfterPointLoad = world.state(handle);
    CHECK(velocityBeforePointLoad.has_value() && velocityAfterPointLoad.has_value());
    CHECK(velocityAfterPointLoad->linearVelocity.y > velocityBeforePointLoad->linearVelocity.y);
    CHECK(!world.apply_force_at_point(
        handle, {0.0F, 1.0F, 0.0F},
        {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F}));
    CHECK(world.apply_impulse_at_point(handle, {1.0F, 0.0F, 0.0F}, {3.0F, 6.0F, -1.0F}));
    world.set_contact_sink(nullptr);
    CHECK(!world.set_contact_material(handle, 7U));
    const auto before = world.state(handle);
    world.step(1.0F / 60.0F);
    const auto after = world.state(handle);
    CHECK(before.has_value() && after.has_value());
    CHECK(after->currentTransform.position.y < before->currentTransform.position.y);
    const RigidTransform midpoint = world.interpolated_transform(handle, 0.5F);
    CHECK(midpoint.position.y <= before->currentTransform.position.y);
    CHECK(midpoint.position.y >= after->currentTransform.position.y);
    CHECK(world.destroy_body(handle));
    CHECK(world.active_body_count() == 0);

    SolverInertiaTensor invalid{-1.0, 1.0, 1.0, 0.0, 0.0, 0.0};
    CHECK(!inertia_is_positive_semidefinite(invalid));
}


} // namespace

int main() {
    try {
        test_coordinate_mapping();
        test_adaptive_brick_encodings();
        test_payload_pools_and_direct_mutation();
        test_brick_mutation_fuzz();
        test_damage_and_determinism();
        test_damage_does_not_create_empty_headers();
        test_quantized_damage_inputs();
        test_surface_reference();
        test_bitwise_surface_matches_reference();
        test_flat_brick_map_and_deterministic_hash();
        test_parallel_surface_jobs_are_deterministic();
        test_gpu_brick_mirror_and_raycast_oracle();
        test_packed_brickmap_and_hdda_oracle();
        test_fence_upload_ring();
        test_queries();
        test_transformed_queries_and_player_safety();
        test_conservative_capsule_sweep_randomized_oracle();
        test_connectivity_and_split_transaction();
        test_incremental_connectivity_cache();
        test_incremental_randomized_oracle();
        test_compact_connectivity_memory_and_csr();
        test_damage_workspace_growth_telemetry();
        test_stale_split_rejected();
        test_box_collision_proxy();
        test_fragment_mass_properties_and_solver_package();
        test_rigid_body_validation_and_split_velocity();
        test_rigid_body_adapter();
        test_work_debt_governor();
    } catch (const std::exception& error) {
        std::cerr << "Unhandled exception: " << error.what() << '\n';
        return 2;
    }

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All destructible voxel core tests passed.\n";
    return 0;
}
