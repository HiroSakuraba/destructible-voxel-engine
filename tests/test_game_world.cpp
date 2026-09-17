#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

#include "dve/dvox.hpp"
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

[[nodiscard]] std::unique_ptr<VoxelObject> make_solid_cube(std::uint64_t id, int size, MaterialId material) {
    auto object = std::make_unique<VoxelObject>(id);
    for (int x = 0; x < size; ++x)
        for (int y = 0; y < size; ++y)
            for (int z = 0; z < size; ++z) object->set_voxel({x, y, z}, material);
    return object;
}

void test_marker_objects() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Waypoint";
    desc.tags = {"ai", "waypoint"};
    desc.transform = make_rigid_transform({1.0F, 2.0F, 3.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    CHECK(id != kInvalidGameObjectId);
    CHECK(world.has_object(id));
    CHECK(world.object_count() == 1);

    const auto pos = world.position(id);
    CHECK(pos.has_value());
    if (pos) CHECK(pos->x == 1.0F && pos->y == 2.0F && pos->z == 3.0F);

    CHECK(world.set_position(id, {5.0F, 5.0F, 5.0F}));
    const auto moved = world.position(id);
    CHECK(moved && moved->x == 5.0F);

    CHECK(world.find_by_name("Waypoint") == id);
    CHECK(!world.find_by_name("Nope").has_value());
    const auto tagged = world.find_by_tag("ai");
    CHECK(tagged.size() == 1 && tagged.front() == id);
    CHECK(world.has_tag(id, "waypoint"));
    CHECK(!world.has_tag(id, "enemy"));

    // A marker has no physics body: velocity/impulse ops are meaningless and must fail cleanly.
    CHECK(!world.linear_velocity(id).has_value());
    CHECK(!world.apply_impulse(id, {0.0F, 1.0F, 0.0F}));

    CHECK(world.destroy_object(id));
    CHECK(!world.has_object(id));
    CHECK(world.object_count() == 0);
}

void test_static_object_is_immovable() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Floor";
    desc.voxelSizeMeters = 0.5F;
    desc.dynamic = false;
    desc.voxels = make_solid_cube(1, 4, 1);
    desc.transform = make_rigid_transform({0.0F, -1.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    CHECK(id != kInvalidGameObjectId);
    CHECK(!world.set_position(id, {10.0F, 10.0F, 10.0F}));
    const auto pos = world.position(id);
    CHECK(pos && pos->y == -1.0F);
}

void test_dynamic_object_impulse_moves_it() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Crate";
    desc.voxelSizeMeters = 0.25F;
    desc.dynamic = true;
    desc.voxels = make_solid_cube(2, 4, 1);
    desc.transform = make_rigid_transform({0.0F, 10.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) std::cerr << "create_object failed: " << error << "\n";
    CHECK(id != kInvalidGameObjectId);

    const auto before = world.position(id);
    CHECK(before.has_value());

    // A strong sideways impulse should visibly move the object sideways within a handful of
    // ticks, with no floor involved (ReferenceRigidBodyWorld does no contact resolution, so
    // this checks the impulse -> velocity -> integration path, not collision).
    CHECK(world.apply_impulse(id, {500.0F, 0.0F, 0.0F}));
    const auto velocityAfterImpulse = world.linear_velocity(id);
    CHECK(velocityAfterImpulse && velocityAfterImpulse->x > 0.0F);

    for (int step = 0; step < 30; ++step) world.tick(1.0F / 60.0F);

    const auto after = world.position(id);
    CHECK(after.has_value());
    if (before && after) CHECK(after->x > before->x + 0.15F);

    CHECK(world.set_linear_velocity(id, {0.0F, 0.0F, 0.0F}));
    const auto stopped = world.linear_velocity(id);
    CHECK(stopped && stopped->x == 0.0F && stopped->y == 0.0F && stopped->z == 0.0F);
}

void test_damage_and_auto_destroy() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Wall";
    desc.voxelSizeMeters = 1.0F;
    desc.dynamic = false;
    desc.voxels = make_solid_cube(1, 4, 1);
    desc.transform = make_rigid_transform({0.0F, 0.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) std::cerr << "create_object failed: " << error << "\n";
    CHECK(id != kInvalidGameObjectId);

    int damageEvents = 0;
    int destroyEvents = 0;
    world.on_damage([&](const GameDamageEvent& event) {
        ++damageEvents;
        CHECK(event.objectId == id);
        CHECK(event.removedVoxelCount > 0);
    });
    world.on_destroyed([&](GameObjectId destroyedId) {
        ++destroyEvents;
        CHECK(destroyedId == id);
    });

    // Center of the object, radius large enough to remove some but not all of a 4x4x4 block.
    const auto removedFirst = world.damage_sphere(id, {2.0F, 2.0F, 2.0F}, 1.2F);
    CHECK(removedFirst.has_value() && *removedFirst > 0);
    CHECK(world.has_object(id));
    CHECK(damageEvents == 1);
    CHECK(destroyEvents == 0);

    // A radius covering the whole remaining block empties it and auto-destroys the object.
    const auto removedSecond = world.damage_sphere(id, {2.0F, 2.0F, 2.0F}, 10.0F);
    CHECK(removedSecond.has_value() && *removedSecond > 0);
    CHECK(!world.has_object(id));
    CHECK(damageEvents == 2);
    CHECK(destroyEvents == 1);

    // Damaging a now-nonexistent object must fail cleanly, not resurrect bookkeeping.
    CHECK(!world.damage_sphere(id, {2.0F, 2.0F, 2.0F}, 1.0F).has_value());
}

void test_raycast_unit_correctness() {
    // The specific regression this guards against: query.cpp's raycast_voxels_transformed
    // takes objectTransform.position and the ray origin in the *same* units with no implicit
    // voxel-size scaling, exactly like build_fragment_solver_package. A non-trivial
    // voxelSizeMeters (not 1.0) and a non-zero object position together are exactly the
    // combination that silently breaks if that conversion is wrong in either direction.
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Target";
    desc.voxelSizeMeters = 0.5F; // deliberately not 1.0
    desc.dynamic = false;
    desc.voxels = make_solid_cube(2001, 4, 1); // occupies local voxel space [0,4)^3
    desc.transform = make_rigid_transform({10.0F, 0.0F, 0.0F}, {}); // deliberately offset, in meters
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) std::cerr << "create_object failed: " << error << "\n";
    CHECK(id != kInvalidGameObjectId);

    // World-space box: [10,12] x [0,2] x [0,2] meters (4 voxels * 0.5 m each along each axis).
    // Fire straight down the -X axis from well outside the box, aimed at its center height.
    const auto hit = world.raycast({20.0F, 1.0F, 1.0F}, {-1.0F, 0.0F, 0.0F}, 100.0F);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->objectId == id);
        // Should hit the +X face of the box at world x = 12.0 m (10.0 base + 4 voxels * 0.5 m).
        CHECK(std::fabs(hit->worldPosition.x - 12.0F) < 1.0e-3F);
        CHECK(std::fabs(hit->worldPosition.y - 1.0F) < 1.0e-3F);
        CHECK(std::fabs(hit->worldPosition.z - 1.0F) < 1.0e-3F);
        // Distance from origin (20,1,1) to the hit (12,1,1) is exactly 8 meters.
        CHECK(std::fabs(hit->distance - 8.0F) < 1.0e-3F);
        CHECK(hit->worldNormal.x > 0.5F); // outward-facing +X normal
    }

    // A ray that misses entirely must return nullopt, not a false positive.
    CHECK(!world.raycast({20.0F, 50.0F, 50.0F}, {-1.0F, 0.0F, 0.0F}, 100.0F).has_value());
}

void test_timers() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    int onceFired = 0;
    int repeatFired = 0;
    (void)world.schedule_once(0.25F, [&] { ++onceFired; });
    const GameWorld::TimerId repeating = world.schedule_repeating(0.1F, [&] { ++repeatFired; });

    constexpr float step = 1.0F / 60.0F; // ~0.0167s
    for (int i = 0; i < 15; ++i) world.tick(step); // ~0.25s elapsed
    CHECK(onceFired == 1);
    CHECK(repeatFired >= 2); // fires at ~0.1s and ~0.2s within this window

    const int repeatCountAtCancel = repeatFired;
    CHECK(world.cancel_timer(repeating));
    for (int i = 0; i < 30; ++i) world.tick(step); // another ~0.5s
    CHECK(repeatFired == repeatCountAtCancel); // no further fires after cancellation
    CHECK(onceFired == 1); // one-shot never fires again either

    CHECK(!world.cancel_timer(repeating)); // cancelling twice fails cleanly
}

void test_fragmentation_on_disconnecting_damage() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    // A "dumbbell": two 3x2x2 blocks (A at x=[0,3), B at x=[4,7)) joined by a single-voxel
    // bridge at (3,0,0), which is 6-connectivity-adjacent to both A's x=2 face and B's x=4
    // face. Damaging just the bridge should disconnect A from B.
    auto voxels = std::make_unique<VoxelObject>(9001);
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) voxels->set_voxel({x, y, z}, 1);
    voxels->set_voxel({3, 0, 0}, 1); // the bridge
    for (int x = 4; x < 7; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) voxels->set_voxel({x, y, z}, 1);
    CHECK(voxels->occupied_voxel_count() == 25); // 12 + 1 + 12

    GameObjectDesc desc;
    desc.name = "Dumbbell";
    desc.voxelSizeMeters = 1.0F;
    desc.dynamic = false; // deliberately static: fragments must still be created as dynamic debris
    desc.voxels = std::move(voxels);
    desc.transform = make_rigid_transform({0.0F, 0.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) std::cerr << "create_object failed: " << error << "\n";
    CHECK(id != kInvalidGameObjectId);
    CHECK(world.object_count() == 1);

    std::vector<GameObjectId> reportedFragments;
    world.on_damage([&](const GameDamageEvent& event) { reportedFragments = event.newFragmentIds; });

    // Center of the bridge voxel, small enough radius to remove only it.
    const auto removed = world.damage_sphere(id, {3.5F, 0.5F, 0.5F}, 0.6F);
    CHECK(removed.has_value() && *removed == 1);

    CHECK(world.has_object(id)); // the larger piece keeps the original id
    CHECK(world.object_count() == 2); // exactly one new fragment
    CHECK(reportedFragments.size() == 1);

    const GameObjectId fragmentId = reportedFragments.empty() ? kInvalidGameObjectId : reportedFragments.front();
    CHECK(fragmentId != kInvalidGameObjectId);
    CHECK(world.has_object(fragmentId));
    CHECK(fragmentId != id);

    // Total remaining voxels across both pieces must equal what was there minus the one
    // removed: no voxels lost or duplicated by the split itself.
    const auto primaryCount = world.voxel_count(id);
    const auto fragmentCount = world.voxel_count(fragmentId);
    CHECK(primaryCount.has_value() && fragmentCount.has_value());
    if (primaryCount && fragmentCount) CHECK(*primaryCount + *fragmentCount == 24);

    // The fragment must be a real, independent dynamic body (falls under gravity), even
    // though the parent object was static.
    const auto fragmentVelocityBefore = world.linear_velocity(fragmentId);
    CHECK(fragmentVelocityBefore.has_value()); // has a body at all, unlike a marker
    CHECK(world.apply_impulse(fragmentId, {0.0F, 5.0F, 0.0F})); // only dynamic bodies accept impulses
}

void test_spawn_asset_uses_real_per_material_density() {
    // Deliberately checks that spawn_asset's mass comes from the *asset's own* per-material
    // densities (build_material_mass_table_from_definitions), not a single uniform value the
    // way create_object/spawn_box use: half the object is a light material, half is a heavy
    // one, and the expected mass is their weighted sum, not (say) the heavy material applied
    // uniformly or an unrelated default density.
    CookedVoxelAsset asset(777);
    asset.voxelSizeMeters = 0.5F;
    VoxelMaterialDefinition air;
    air.name = "Air";
    air.densityKilogramsPerCubicMeter = 0.0F;
    asset.materials.push_back(air);
    VoxelMaterialDefinition light;
    light.name = "Light";
    light.densityKilogramsPerCubicMeter = 500.0F;
    asset.materials.push_back(light);
    VoxelMaterialDefinition heavy;
    heavy.name = "Heavy";
    heavy.densityKilogramsPerCubicMeter = 5000.0F;
    asset.materials.push_back(heavy);

    // A 4x2x2 block: x in [0,2) is Light (material 1), x in [2,4) is Heavy (material 2).
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) asset.object.set_voxel({x, y, z}, 1);
    for (int x = 2; x < 4; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) asset.object.set_voxel({x, y, z}, 2);
    CHECK(asset.object.occupied_voxel_count() == 16);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "dve_game_world_test_asset.dvox";
    std::string writeError;
    CHECK(write_dvox(path, asset, {}, &writeError));

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::string error;
    const GameObjectId id = world.spawn_asset(path, "MixedDensityBlock", make_rigid_transform({0.0F, 0.0F, 0.0F}, {}),
                                               /*dynamic=*/true, /*structural=*/true, &error);
    if (id == kInvalidGameObjectId) std::cerr << "spawn_asset failed: " << error << "\n";
    CHECK(id != kInvalidGameObjectId);
    CHECK(world.voxel_count(id) == 16U);

    // Expected mass: 8 voxels light + 8 voxels heavy, each voxel 0.5^3 = 0.125 m^3.
    const double expectedMass = 8.0 * 500.0 * 0.125 + 8.0 * 5000.0 * 0.125; // = 500 + 5000 = 5500 kg
    const Float3 impulse{static_cast<float>(expectedMass) * 2.0F, 0.0F, 0.0F}; // should give v = 2 m/s if mass is right
    CHECK(world.apply_impulse(id, impulse));
    const auto velocity = world.linear_velocity(id);
    CHECK(velocity.has_value());
    if (velocity) {
        std::cerr.flush();
        // Generous tolerance: density-quantization (4095 discrete steps between 0 and the
        // heaviest material present) introduces a small, expected rounding error, not a bug.
        CHECK(std::fabs(velocity->x - 2.0F) < 0.05F);
    }

    // A static spawn from the same asset must also work (no mass/velocity involved at all).
    std::string staticError;
    const GameObjectId staticId = world.spawn_asset(
        path, "StaticMixedBlock", make_rigid_transform({10.0F, 0.0F, 0.0F}, {}), /*dynamic=*/false, true, &staticError);
    if (staticId == kInvalidGameObjectId) std::cerr << "static spawn_asset failed: " << staticError << "\n";
    CHECK(staticId != kInvalidGameObjectId);
    CHECK(!world.set_position(staticId, {99.0F, 0.0F, 0.0F})); // static: immovable, same as create_object's static path

    std::filesystem::remove(path);
}

void test_on_tick_listener() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    int tickCount = 0;
    float lastDelta = 0.0F;
    world.on_tick([&](float dt) { ++tickCount; lastDelta = dt; });
    for (int i = 0; i < 5; ++i) world.tick(1.0F / 30.0F);
    CHECK(tickCount == 5);
    CHECK(std::fabs(lastDelta - 1.0F / 30.0F) < 1.0e-6F);
}

void test_lifecycle_memberships_and_deterministic_pooling() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::vector<GameLifecycleEvent> events;
    world.on_lifecycle([&](const GameLifecycleEvent& event) { events.push_back(event); });

    GameObjectDesc member;
    member.name = "Enemy Turret";
    member.tags = {"enemy", "turret", "enemy"};
    member.groups = {"defenses"};
    member.layer = 6U;
    const GameObjectId memberId = world.create_object(std::move(member));
    CHECK(memberId != kInvalidGameObjectId);
    CHECK(world.find_by_tag("enemy") == std::vector<GameObjectId>{memberId});
    CHECK(world.find_by_group("defenses") == std::vector<GameObjectId>{memberId});
    CHECK(world.find_by_layer(6U) == std::vector<GameObjectId>{memberId});
    CHECK(!events.empty() && events.back().kind == GameLifecycleEventKind::Spawn);
    CHECK(world.set_enabled(memberId, false));
    CHECK(world.find_by_tag("enemy").empty());
    CHECK(events.back().kind == GameLifecycleEventKind::Disable);
    CHECK(world.set_enabled(memberId, true));
    CHECK(events.back().kind == GameLifecycleEventKind::Enable);

    Component lifecycleComponent;
    lifecycleComponent.type = "game.lifecycle_probe";
    lifecycleComponent.properties.emplace("active", true);
    std::string error;
    Component* added = world.add_component(memberId, std::move(lifecycleComponent), &error);
    CHECK(added != nullptr);
    const ComponentId componentId = added ? added->id : kInvalidComponentId;
    CHECK(events.back().kind == GameLifecycleEventKind::ComponentAdded);
    CHECK(world.set_component_enabled(memberId, componentId, false));
    CHECK(events.back().kind == GameLifecycleEventKind::ComponentDisabled);
    CHECK(world.remove_component(memberId, componentId));
    CHECK(events.back().kind == GameLifecycleEventKind::ComponentRemoved);

    GameObjectPoolDesc poolDesc;
    poolDesc.name = "projectiles";
    poolDesc.capacity = 2U;
    poolDesc.prototype.name = "Projectile";
    poolDesc.prototype.tags = {"projectile"};
    const GameObjectPoolId poolId = world.register_pool(std::move(poolDesc), &error);
    CHECK(poolId != kInvalidGameObjectPoolId);
    CHECK(world.pool_available(poolId) == 2U);
    const GameObjectId first = world.acquire_from_pool(poolId, make_rigid_transform({1.0F, 0.0F, 0.0F}, {}), &error);
    const GameObjectId second = world.acquire_from_pool(poolId, make_rigid_transform({2.0F, 0.0F, 0.0F}, {}), &error);
    CHECK(first != kInvalidGameObjectId && second != kInvalidGameObjectId && first < second);
    CHECK(world.pool_available(poolId) == 0U);
    CHECK(world.acquire_from_pool(poolId, {}, &error) == kInvalidGameObjectId);
    CHECK(world.release_to_pool(first, &error));
    CHECK(world.pool_available(poolId) == 1U);
    const GameObjectId recycled = world.acquire_from_pool(poolId, make_rigid_transform({3.0F, 0.0F, 0.0F}, {}), &error);
    CHECK(recycled == first);
    CHECK(world.destroy_object(second)); // pooled destroy means deterministic release.
    CHECK(world.pool_available(poolId) == 1U);
    CHECK(std::any_of(events.begin(), events.end(), [](const GameLifecycleEvent& event) {
        return event.kind == GameLifecycleEventKind::PoolAcquire && event.poolName == "projectiles";
    }));
    CHECK(std::any_of(events.begin(), events.end(), [](const GameLifecycleEvent& event) {
        return event.kind == GameLifecycleEventKind::PoolRelease && event.poolName == "projectiles";
    }));
}


} // namespace

int main() {
    test_marker_objects();
    test_static_object_is_immovable();
    test_dynamic_object_impulse_moves_it();
    test_damage_and_auto_destroy();
    test_fragmentation_on_disconnecting_damage();
    test_spawn_asset_uses_real_per_material_density();
    test_raycast_unit_correctness();
    test_timers();
    test_on_tick_listener();
    test_lifecycle_memberships_and_deterministic_pooling();

    if (failures == 0) {
        std::cout << "dve_game_world_tests: PASS\n";
        return 0;
    }
    std::cerr << "dve_game_world_tests: " << failures << " FAILURE(S)\n";
    return 1;
}
