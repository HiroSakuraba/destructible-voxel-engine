// Fix verification tests for the Phase 4 critique physics items.
// Covers:
//  9a. apply_angular_impulse rotates the body-frame inertia into the world frame
//      (R * I_body^-1 * R^T); axis-aligned bodies behave exactly as before.
//  9b. damage_sphere clamps huge finite radii (no batch-loop hang); NaN,
//      infinite and non-positive radii are still rejected as before.
//  10. set_contact_material succeeds on valid handles (regression guard for the
//      updated stale assertions).

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <string>

#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/transform.hpp"

namespace {

int failures = 0;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ << "\n";                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

[[nodiscard]] dve::RigidBodyHandle make_test_body(
    dve::ReferenceRigidBodyWorld& world,
    dve::Quaternion rotation,
    dve::SolverInertiaTensor inertia) {
    using namespace dve;
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform({0.0F, 0.0F, 0.0F}, rotation);
    desc.massKilograms = 1.0;
    desc.inertiaKilogramMetersSquared = inertia;
    desc.boxes.push_back({{}, {0.5F, 0.5F, 0.5F}});
    return world.create_body(desc);
}

// 9a: a rotated body must see the world-frame inverse inertia R * I^-1 * R^T,
// not the body-frame I^-1 applied directly to the world-space impulse.
void test_angular_impulse_rotated_body() {
    using namespace dve;
    ReferenceRigidBodyWorld world;
    world.set_gravity({});

    // 90 degrees about +Z; anisotropic inertia so a frame error is detectable.
    const Quaternion rotation =
        quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, std::numbers::pi_v<float> * 0.5F);
    const RigidBodyHandle handle =
        make_test_body(world, rotation, {1.0, 2.0, 3.0, 0.0, 0.0, 0.0});
    CHECK(handle != kInvalidRigidBodyHandle);

    // World-space impulse along +Y. R_z(90deg) maps body +X to world +Y, so:
    //   R^T * +Y = +X (body), I^-1 * +X = (1,0,0), R * (1,0,0) = +Y (world).
    // The old frame-blind code returned I^-1 * J = (0, 0.5, 0) instead.
    CHECK(world.apply_angular_impulse(handle, {0.0F, 1.0F, 0.0F}));
    const auto state = world.state(handle);
    CHECK(state.has_value());
    if (!state) return;
    const Float3 expected{0.0F, 1.0F, 0.0F};
    const Float3 frameBlind{0.0F, 0.5F, 0.0F};
    CHECK(length(subtract(state->angularVelocity, expected)) < 1.0e-4F);
    CHECK(length(subtract(state->angularVelocity, frameBlind)) > 0.25F);
}

// 9a companion: an axis-aligned body must behave exactly as before the fix
// (R is identity, so R * I^-1 * R^T == I^-1).
void test_angular_impulse_axis_aligned_unchanged() {
    using namespace dve;
    ReferenceRigidBodyWorld world;
    world.set_gravity({});
    const RigidBodyHandle handle =
        make_test_body(world, {}, {2.0, 4.0, 8.0, 0.0, 0.0, 0.0});
    CHECK(handle != kInvalidRigidBodyHandle);
    CHECK(world.apply_angular_impulse(handle, {2.0F, 4.0F, 8.0F}));
    const auto state = world.state(handle);
    CHECK(state.has_value());
    if (!state) return;
    CHECK(length(subtract(state->angularVelocity, Float3{1.0F, 1.0F, 1.0F})) < 1.0e-6F);
}

// 9b: a huge finite radius is clamped at the damage_sphere boundary, so
// build_damage_batches never sees ~INT32_MAX subvoxel units per axis.
void test_damage_sphere_huge_radius_clamped() {
    using namespace dve;
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    auto voxels = std::make_unique<VoxelObject>(4242);
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y)
            for (int z = 0; z < 4; ++z) voxels->set_voxel({x, y, z}, 1);
    GameObjectDesc desc;
    desc.name = "ClampTarget";
    desc.voxelSizeMeters = 10.0F; // coarse voxels: keeps the clamped batch walk trivial
    desc.dynamic = false;
    desc.voxels = std::move(voxels);
    desc.transform = make_rigid_transform({0.0F, 0.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) std::cerr << "create_object failed: " << error << "\n";
    CHECK(id != kInvalidGameObjectId);
    if (id == kInvalidGameObjectId) return;

    // NaN / infinite / non-positive radii are still rejected, exactly as before.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(!world.damage_sphere(id, {20.0F, 20.0F, 20.0F}, nan).has_value());
    CHECK(!world.damage_sphere(id, {20.0F, 20.0F, 20.0F}, inf).has_value());
    CHECK(!world.damage_sphere(id, {20.0F, 20.0F, 20.0F}, 0.0F).has_value());
    CHECK(!world.damage_sphere(id, {20.0F, 20.0F, 20.0F}, -5.0F).has_value());

    float reportedRadius = -1.0F;
    world.on_damage([&](const GameDamageEvent& event) { reportedRadius = event.radius; });

    const auto start = std::chrono::steady_clock::now();
    const auto removed = world.damage_sphere(id, {20.0F, 20.0F, 20.0F}, 1.0e20F);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(removed.has_value());
    if (removed) CHECK(*removed == 64); // the whole 4x4x4 cube, nothing more
    CHECK(reportedRadius > 0.0F);
    CHECK(reportedRadius <= 1000.0F); // kMaxDamageSphereRadiusMeters
    CHECK(elapsed < std::chrono::seconds(30)); // must complete, not hang
}

// 10: regression guard — the reference backend stores contact materials now.
void test_set_contact_material_succeeds_on_valid_handle() {
    using namespace dve;
    ReferenceRigidBodyWorld world;
    world.set_gravity({});
    const RigidBodyHandle handle =
        make_test_body(world, {}, {1.0, 1.0, 1.0, 0.0, 0.0, 0.0});
    CHECK(handle != kInvalidRigidBodyHandle);
    CHECK(world.set_contact_material(handle, 7U));
    CHECK(!world.set_contact_material(kInvalidRigidBodyHandle, 7U));
}

} // namespace

int main() {
    test_angular_impulse_rotated_body();
    test_angular_impulse_axis_aligned_unchanged();
    test_damage_sphere_huge_radius_clamped();
    test_set_contact_material_succeeds_on_valid_handle();
    if (failures == 0) std::cout << "DVE critique physics fix tests passed\n";
    return failures == 0 ? 0 : 1;
}
