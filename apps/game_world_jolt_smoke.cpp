// Validates GameWorld against a real Jolt Physics backend: a dynamic crate falls under real
// gravity, collides with a real static floor, settles, and sleeps; then a raycast finds it,
// and enough damage to empty it triggers automatic destruction with a real physics body
// teardown. tests/test_game_world.cpp already covers the object-model/timer/event logic
// against the physics-independent reference backend; this is specifically about the seam
// with a real physics engine.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "dve/game_world.hpp"
#include "dve/physics_jolt_backend.hpp"

namespace {

int failures = 0;

#define CHECK(...)                                                                        \
    do {                                                                                  \
        if (!(__VA_ARGS__)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);   \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

using namespace dve;

std::unique_ptr<VoxelObject> make_solid_cube(std::uint64_t id, int size, MaterialId material) {
    auto object = std::make_unique<VoxelObject>(id);
    for (int x = 0; x < size; ++x)
        for (int y = 0; y < size; ++y)
            for (int z = 0; z < size; ++z) object->set_voxel({x, y, z}, material);
    return object;
}

std::unique_ptr<VoxelObject> make_solid_slab(std::uint64_t id, int width, int height, int depth, MaterialId material) {
    auto object = std::make_unique<VoxelObject>(id);
    for (int x = 0; x < width; ++x)
        for (int y = 0; y < height; ++y)
            for (int z = 0; z < depth; ++z) object->set_voxel({x, y, z}, material);
    return object;
}

} // namespace

int main() {
    GameWorld world(std::make_unique<JoltRigidBodyWorld>());

    GameObjectDesc floorDesc;
    floorDesc.name = "Floor";
    floorDesc.voxelSizeMeters = 0.5F;
    floorDesc.dynamic = false;
    // A thin slab (2 voxels tall), not a solid cube: the crate below is dropped from y=3 and
    // must land ON TOP of this, not spawn buried inside a much taller "floor."
    floorDesc.voxels = make_solid_slab(1, 20, 2, 20, 1);
    floorDesc.transform = make_rigid_transform({-5.0F, -1.0F, -5.0F}, {});
    std::string error;
    const GameObjectId floorId = world.create_object(std::move(floorDesc), &error);
    CHECK(floorId != kInvalidGameObjectId);
    if (floorId == kInvalidGameObjectId) std::fprintf(stderr, "floor error: %s\n", error.c_str());

    GameObjectDesc crateDesc;
    crateDesc.name = "Crate";
    crateDesc.voxelSizeMeters = 0.25F;
    crateDesc.dynamic = true;
    crateDesc.voxels = make_solid_cube(2, 4, 1);
    crateDesc.transform = make_rigid_transform({0.0F, 3.0F, 0.0F}, {});
    const GameObjectId crateId = world.create_object(std::move(crateDesc), &error);
    CHECK(crateId != kInvalidGameObjectId);
    if (crateId == kInvalidGameObjectId) std::fprintf(stderr, "crate error: %s\n", error.c_str());

    int tickCount = 0;
    world.on_tick([&](float) { ++tickCount; });

    constexpr float kFixedDelta = 1.0F / 120.0F;
    bool settled = false;
    for (int step = 0; step < 120 * 6 && !settled; ++step) {
        world.tick(kFixedDelta);
        const auto vel = world.linear_velocity(crateId);
        if (vel && std::fabs(vel->x) < 0.01F && std::fabs(vel->y) < 0.01F && std::fabs(vel->z) < 0.01F) settled = true;
    }
    CHECK(settled);
    CHECK(tickCount > 0);

    const auto restPosition = world.position(crateId);
    CHECK(restPosition.has_value());
    if (restPosition) {
        std::printf("crate_rest_y=%.4f\n", static_cast<double>(restPosition->y));
        // position() reports the object's origin corner (see GameWorld::resolve_transform),
        // not its center: a 4-voxel (1 m) cube created at y=3 has its bottom face, not its
        // center, resting on the floor, so the origin corner should settle near y=0.
        CHECK(std::fabs(restPosition->y - 0.0F) < 0.05F);
    }

    // Raycast straight down through the crate's actual footprint (it spans x:[0,1], z:[0,1]
    // from its origin corner, so aim at the middle of that, not at x=0/z=0 which grazes its edge).
    const auto hit = world.raycast({0.5F, 5.0F, 0.5F}, {0.0F, -1.0F, 0.0F}, 20.0F);
    CHECK(hit.has_value());
    if (hit) CHECK(hit->objectId == crateId);

    // Enough damage to empty the crate destroys it, including its physics body.
    int destroyedCount = 0;
    world.on_destroyed([&](GameObjectId id) { if (id == crateId) ++destroyedCount; });
    const auto removed = world.damage_sphere(crateId, *restPosition, 5.0F);
    CHECK(removed.has_value() && *removed > 0);
    CHECK(!world.has_object(crateId));
    CHECK(destroyedCount == 1);

    // Stepping after destruction must not crash or resurrect the object.
    world.tick(kFixedDelta);
    CHECK(!world.has_object(crateId));

    // Fragmentation against real Jolt: a static bar, cut in the middle, must produce a
    // second body that actually falls under real gravity (not just get created and sit
    // there), proving the split-piece's rigid body was built and registered correctly.
    GameObjectDesc barDesc;
    barDesc.name = "Bar";
    barDesc.voxelSizeMeters = 0.5F;
    barDesc.dynamic = false;
    barDesc.voxels = make_solid_slab(3, 9, 1, 1, 1);
    barDesc.transform = make_rigid_transform({-5.0F, 5.0F, 0.0F}, {}); // well clear of the floor/crate debris
    const GameObjectId barId = world.create_object(std::move(barDesc), &error);
    CHECK(barId != kInvalidGameObjectId);

    std::vector<GameObjectId> barFragments;
    world.on_damage([&](const GameDamageEvent& event) {
        if (event.objectId == barId) barFragments = event.newFragmentIds;
    });
    // Bar spans local x=[0,9) at 0.5 m/voxel = world x=[-5,-0.5]; center voxel (x=4) world
    // center is at -5 + 4.5*0.5 = -2.75.
    const auto removedFromBar = world.damage_sphere(barId, {-2.75F, 5.25F, 0.25F}, 0.3F);
    CHECK(removedFromBar.has_value() && *removedFromBar > 0);
    CHECK(barFragments.size() == 1);

    if (!barFragments.empty()) {
        const GameObjectId fragmentId = barFragments.front();
        const auto fragmentStartY = world.position(fragmentId);
        CHECK(fragmentStartY.has_value());
        for (int step = 0; step < 30; ++step) world.tick(kFixedDelta); // 0.25 s of real gravity
        const auto fragmentAfterY = world.position(fragmentId);
        CHECK(fragmentAfterY.has_value());
        if (fragmentStartY && fragmentAfterY) {
            std::printf("fragment_fall_delta_y=%.4f\n", static_cast<double>(fragmentAfterY->y - fragmentStartY->y));
            CHECK(fragmentAfterY->y < fragmentStartY->y - 0.01F); // actually fell, not just spawned in place
        }
    }

    if (failures == 0) {
        std::printf("dve_game_world_jolt_smoke: PASS\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "dve_game_world_jolt_smoke: %d FAILURE(S)\n", failures);
    return EXIT_FAILURE;
}
