// Bug-hunt probes (2026-10-08): adversarial cases around the debris cap,
// multi-component splits, and the queued destruction path. Written against
// current main (post PRs #79/#80). Each probe states the invariant it checks;
// memory safety is judged by the ASan/UBSan build these run under.
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#include "dve/game_world.hpp"

namespace {

int failures = 0;

#define CHECK(...)                                                                        \
    do {                                                                                  \
        if (!(__VA_ARGS__)) {                                                             \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n"; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

using namespace dve;

GameObjectId spawn(GameWorld& world, std::uint64_t seed, std::unique_ptr<VoxelObject> voxels,
                   float originX = 0.0F) {
    GameObjectDesc desc;
    desc.name = "Probe";
    desc.voxelSizeMeters = 1.0F;
    desc.dynamic = false;
    desc.voxels = std::move(voxels);
    desc.transform = make_rigid_transform({originX, 0.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    CHECK(id != kInvalidGameObjectId);
    return id;
}

void block(std::unique_ptr<VoxelObject>& voxels, int x0, int y0, int z0, int sx, int sy, int sz) {
    for (int x = x0; x < x0 + sx; ++x)
        for (int y = y0; y < y0 + sy; ++y)
            for (int z = z0; z < z0 + sz; ++z) voxels->set_voxel({x, y, z}, 1);
}

// A solid 3x3x3 block joined by a 1-voxel bridge to a dumbbell
// (3x2x2 - bridge - 3x2x2). Breaking the first bridge detaches the whole
// dumbbell as ONE debris fragment that is itself splittable.
GameObjectId spawn_block_and_dumbbell(GameWorld& world, std::uint64_t seed) {
    auto voxels = std::make_unique<VoxelObject>(seed);
    block(voxels, 0, 0, 0, 3, 3, 3);          // 27, stays with the original
    voxels->set_voxel({3, 0, 0}, 1);          // bridge 1
    block(voxels, 4, 0, 0, 3, 2, 2);          // dumbbell end A: 12
    voxels->set_voxel({7, 0, 0}, 1);          // bridge 2 (inside the dumbbell)
    block(voxels, 8, 0, 0, 3, 2, 2);          // dumbbell end B: 12
    return spawn(world, seed, std::move(voxels));
}

std::uint64_t total_voxels(GameWorld& world) {
    std::uint64_t total = 0;
    for (const GameObjectId id : world.object_ids()) {
        const auto count = world.voxel_count(id);
        CHECK(count.has_value());
        if (count) total += *count;
    }
    return total;
}

// Probe A: re-damage the only debris body while the cap is full, on the
// synchronous path. The split must not retire the object being split
// (that was a use-after-free pattern), the new fragment is discarded, and
// the debris body keeps its largest piece.
void test_redamage_debris_at_full_cap_sync() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    world.set_debris_limit(1);
    std::vector<GameObjectId> fragments;
    world.on_damage([&](const GameDamageEvent& event) {
        for (const GameObjectId fragment : event.newFragmentIds) fragments.push_back(fragment);
    });
    const GameObjectId parent = spawn_block_and_dumbbell(world, 31001);
    const auto removed = world.damage_sphere(parent, {3.5F, 0.5F, 0.5F}, 0.6F);
    CHECK(removed.has_value() && *removed == 1);
    CHECK(fragments.size() == 1);
    CHECK(world.debris_count() == 1);
    if (fragments.empty()) return;
    const GameObjectId debris = fragments.front();
    const auto debrisVoxels = world.voxel_count(debris);
    CHECK(debrisVoxels.has_value() && *debrisVoxels == 25);

    // Split the debris itself at its own bridge, cap still full.
    const auto removed2 = world.damage_sphere(debris, {7.5F, 0.5F, 0.5F}, 0.6F);
    CHECK(removed2.has_value() && *removed2 == 1);
    CHECK(world.has_object(debris));       // must not have retired itself
    CHECK(world.debris_count() == 1);
    CHECK(world.object_count() == 2);      // second fragment was discarded
    const auto kept = world.voxel_count(debris);
    CHECK(kept.has_value() && *kept == 12); // largest piece stays

    // Destroying the debris outright drops the count cleanly.
    const auto removed3 = world.damage_sphere(debris, {5.5F, 0.5F, 0.5F}, 100.0F);
    CHECK(removed3.has_value());
    CHECK(!world.has_object(debris));
    CHECK(world.debris_count() == 0);
    CHECK(world.has_object(parent));
}

// Probe B: same re-damage, but through the queued/resumable path.
void test_redamage_debris_at_full_cap_queued() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    world.set_debris_limit(1);
    std::vector<GameObjectId> fragments;
    world.on_damage([&](const GameDamageEvent& event) {
        for (const GameObjectId fragment : event.newFragmentIds) fragments.push_back(fragment);
    });
    const GameObjectId parent = spawn_block_and_dumbbell(world, 31002);
    const auto removed = world.damage_sphere(parent, {3.5F, 0.5F, 0.5F}, 0.6F);
    CHECK(removed.has_value() && *removed == 1);
    CHECK(fragments.size() == 1);
    if (fragments.empty()) return;
    const GameObjectId debris = fragments.front();

    const auto request = world.queue_damage_sphere(debris, {7.5F, 0.5F, 0.5F}, 0.6F);
    CHECK(request.has_value());
    for (int tick = 0; tick < 120; ++tick) world.tick(1.0F / 60.0F);
    CHECK(world.has_object(debris));
    CHECK(world.debris_count() == 1);
    CHECK(world.object_count() == 2);
    const auto kept = world.voxel_count(debris);
    std::cout << "probe B: debris kept voxels = " << (kept ? std::to_string(*kept) : std::string("none"))
              << ", objects = " << world.object_count()
              << ", rejected = " << world.rejected_destruction_requests() << "\n";
    CHECK(kept.has_value() && *kept == 12);
}

// Probe C: one hit detaching three components at once from a shared brick.
// Voxel conservation must be exact: nothing double-removed or duplicated
// by the batch split commit.
void test_multi_component_split_conservation() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    auto voxels = std::make_unique<VoxelObject>(31003);
    voxels->set_voxel({4, 0, 4}, 1); // hub
    voxels->set_voxel({5, 0, 4}, 1);
    voxels->set_voxel({6, 0, 4}, 1); // +x arm
    voxels->set_voxel({3, 0, 4}, 1);
    voxels->set_voxel({2, 0, 4}, 1); // -x arm
    voxels->set_voxel({4, 0, 5}, 1);
    voxels->set_voxel({4, 0, 6}, 1); // +z arm
    voxels->set_voxel({4, 1, 4}, 1);
    voxels->set_voxel({4, 2, 4}, 1); // column above the hub
    const GameObjectId parent = spawn(world, 31003, std::move(voxels));
    CHECK(total_voxels(world) == 9);

    std::vector<GameObjectId> fragments;
    world.on_damage([&](const GameDamageEvent& event) {
        for (const GameObjectId fragment : event.newFragmentIds) fragments.push_back(fragment);
    });
    const auto removed = world.damage_sphere(parent, {4.5F, 0.5F, 4.5F}, 0.6F);
    CHECK(removed.has_value() && *removed == 1); // hub only
    CHECK(fragments.size() == 3);
    CHECK(world.object_count() == 4);
    CHECK(world.debris_count() == 3);
    CHECK(total_voxels(world) == 8); // 9 - 1 removed, none lost or duplicated
}

// Probe D: shrink the cap to zero while destruction is queued, then let
// the queue drain. Nothing may be created, nothing may crash, and the
// parent's carve still lands.
void test_queued_damage_after_cap_zeroed() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::vector<GameObjectId> fragments;
    world.on_damage([&](const GameDamageEvent& event) {
        for (const GameObjectId fragment : event.newFragmentIds) fragments.push_back(fragment);
    });
    const GameObjectId parent = spawn_block_and_dumbbell(world, 31004);
    const auto request = world.queue_damage_sphere(parent, {3.5F, 0.5F, 0.5F}, 0.6F);
    CHECK(request.has_value());
    world.set_debris_limit(0);
    for (int tick = 0; tick < 120; ++tick) world.tick(1.0F / 60.0F);
    CHECK(world.has_object(parent));
    CHECK(world.debris_count() == 0);
    CHECK(fragments.empty());
    const auto kept = world.voxel_count(parent);
    std::cout << "probe D: parent kept voxels = " << (kept ? std::to_string(*kept) : std::string("none"))
              << ", total = " << total_voxels(world)
              << ", rejected = " << world.rejected_destruction_requests() << "\n";
    CHECK(kept.has_value() && *kept == 27); // largest piece: the solid block
    CHECK(total_voxels(world) == 27);
}

} // namespace

int main() {
    test_redamage_debris_at_full_cap_sync();
    test_redamage_debris_at_full_cap_queued();
    test_multi_component_split_conservation();
    test_queued_damage_after_cap_zeroed();
    if (failures == 0) {
        std::cout << "dve_bug_hunt_probes: PASS\n";
        return 0;
    }
    std::cerr << "dve_bug_hunt_probes: " << failures << " failure(s)\n";
    return 1;
}
