#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "dve/physics3d_backend.hpp"
#include "dve/physics_jolt_backend.hpp"
#include "dve/transform.hpp"

namespace {
int failures = 0;
#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (false)

using namespace dve;

RigidBodyCreateDesc make_box(Float3 position, Float3 halfExtents = {0.25F, 0.25F, 0.25F}) {
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform(position, {});
    desc.massKilograms = 2.0;
    const double x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    desc.inertiaKilogramMetersSquared = {
        desc.massKilograms / 3.0 * (y * y + z * z),
        desc.massKilograms / 3.0 * (x * x + z * z),
        desc.massKilograms / 3.0 * (x * x + y * y), 0.0, 0.0, 0.0};
    desc.boxes.push_back({{}, halfExtents});
    return desc;
}

void test_factory_configuration() {
    Physics3DWorldConfig config;
    config.workerThreads = 1U;
    config.subStepCount = 3U;
    config.velocityIterations = 12U;
    config.positionIterations = 4U;
    config.deterministicSimulation = false;
    config.allowSleeping = false;

    Physics3DBackend resolved = Physics3DBackend::Reference;
    std::string error;
    std::unique_ptr<IRigidBodyWorld> base = create_physics3d_world(
        Physics3DBackend::Jolt, &resolved, &error, config);
    REQUIRE(base != nullptr);
    REQUIRE(error.empty());
    REQUIRE(resolved == Physics3DBackend::Jolt);
    auto* world = dynamic_cast<JoltRigidBodyWorld*>(base.get());
    REQUIRE(world != nullptr);
    if (world != nullptr) {
        const JoltPhysicsTelemetry telemetry = world->telemetry();
        REQUIRE(telemetry.configuredWorkerThreads == 1U);
        REQUIRE(telemetry.configuredCollisionSteps == 3U);
        REQUIRE(telemetry.configuredVelocityIterations == 12U);
        REQUIRE(telemetry.configuredPositionIterations == 4U);
        REQUIRE(!telemetry.deterministicSimulation);
        REQUIRE(!telemetry.sleepingAllowed);
    }
}

void test_mesh_queries_and_casts() {
    JoltWorldConfig config;
    config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    world.set_gravity({0.0F, -9.81F, 0.0F});

    const std::array<Float3, 4> vertices{{
        {-5.0F, 0.0F, -5.0F}, {5.0F, 0.0F, -5.0F},
        {5.0F, 0.0F, 5.0F}, {-5.0F, 0.0F, 5.0F}}};
    // Winding faces upward.
    const std::array<std::uint32_t, 6> indices{{0U, 2U, 1U, 0U, 3U, 2U}};
    const RigidBodyHandle mesh = world.create_static_triangle_mesh(
        make_rigid_transform({}, {}), vertices, indices, 27U);
    REQUIRE(mesh != kInvalidRigidBodyHandle);
    world.optimize_broad_phase();

    const auto ray = world.ray_cast_closest({0.0F, 3.0F, 0.0F}, {0.0F, -6.0F, 0.0F});
    REQUIRE(ray.has_value());
    if (ray) {
        REQUIRE(ray->body == mesh);
        REQUIRE(ray->material == 27U);
        REQUIRE(std::fabs(ray->point.y) < 0.01F);
        REQUIRE(ray->normal.y > 0.9F);
        REQUIRE(ray->fraction > 0.45F && ray->fraction < 0.55F);
    }

    std::array<JoltOverlapHit, 8> overlaps{};
    const std::size_t overlapCount = world.query_aabb(
        {{-1.0F, -0.1F, -1.0F}, {1.0F, 0.1F, 1.0F}}, overlaps);
    REQUIRE(overlapCount >= 1U);
    bool foundMesh = false;
    for (std::size_t i = 0; i < overlapCount; ++i) {
        if (overlaps[i].body == mesh && overlaps[i].material == 27U) foundMesh = true;
    }
    REQUIRE(foundMesh);

    const auto sphereCast = world.cast_sphere_closest(
        {0.0F, 3.0F, 0.0F}, 0.5F, {0.0F, -6.0F, 0.0F});
    REQUIRE(sphereCast.has_value());
    if (sphereCast) {
        REQUIRE(sphereCast->body == mesh);
        REQUIRE(sphereCast->normal.y > 0.9F);
        REQUIRE(sphereCast->fraction > 0.35F && sphereCast->fraction < 0.5F);
    }

    const auto telemetry = world.telemetry();
    REQUIRE(telemetry.versionMajor == 5U);
    REQUIRE(telemetry.versionMinor == 6U);
    REQUIRE(telemetry.rayQueries == 1U);
    REQUIRE(telemetry.overlapQueries == 1U);
    REQUIRE(telemetry.shapeCasts == 1U);
    REQUIRE(world.destroy_body(mesh));
}

void test_snapshot_round_trip() {
    JoltWorldConfig config;
    config.workerThreads = 1U;
    config.deterministicSimulation = true;
    JoltRigidBodyWorld world(config);
    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -0.5F, 0.0F}, {}), {5.0F, 0.5F, 5.0F});
    const RigidBodyHandle box = world.create_body(make_box({0.0F, 3.0F, 0.0F}));
    REQUIRE(floor != kInvalidRigidBodyHandle && box != kInvalidRigidBodyHandle);

    constexpr float dt = 1.0F / 120.0F;
    for (int i = 0; i < 30; ++i) world.step(dt);
    const auto snapshot = world.save_snapshot();
    REQUIRE(snapshot.has_value());
    const auto saved = world.state(box);
    REQUIRE(saved.has_value());
    for (int i = 0; i < 60; ++i) world.step(dt);
    const auto moved = world.state(box);
    REQUIRE(moved.has_value());
    if (saved && moved) REQUIRE(std::fabs(moved->currentTransform.position.y - saved->currentTransform.position.y) > 0.1F);

    REQUIRE(snapshot && world.restore_snapshot(*snapshot));
    const auto restored = world.state(box);
    REQUIRE(restored.has_value());
    if (saved && restored) {
        REQUIRE(std::fabs(restored->currentTransform.position.y - saved->currentTransform.position.y) < 1.0e-5F);
        REQUIRE(std::fabs(restored->linearVelocity.y - saved->linearVelocity.y) < 1.0e-5F);
    }
    const auto telemetry = world.telemetry();
    REQUIRE(telemetry.snapshotsSaved == 1U);
    REQUIRE(telemetry.snapshotsRestored == 1U);
}

void test_virtual_character() {
    JoltWorldConfig config;
    config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -0.5F, 0.0F}, {}), {10.0F, 0.5F, 10.0F});
    REQUIRE(floor != kInvalidRigidBodyHandle);

    JoltVirtualCharacterDesc desc;
    desc.transform = make_rigid_transform({0.0F, 1.5F, 0.0F}, {});
    const JoltVirtualCharacterHandle character = world.create_virtual_character(desc);
    REQUIRE(character != kInvalidJoltVirtualCharacterHandle);

    constexpr float dt = 1.0F / 120.0F;
    JoltVirtualCharacterInput input;
    input.desiredHorizontalVelocity = {1.5F, 0.0F, 0.0F};
    for (int i = 0; i < 240; ++i) {
        REQUIRE(world.update_virtual_character(character, input, dt));
        world.step(dt);
    }
    auto state = world.virtual_character_state(character);
    REQUIRE(state.has_value());
    if (state) {
        REQUIRE(state->transform.position.x > 1.0F);
        REQUIRE(state->groundState == JoltCharacterGroundState::OnGround);
        REQUIRE(state->groundBody == floor);
    }

    input.jump = true;
    input.jumpSpeedMetersPerSecond = 5.0F;
    REQUIRE(world.update_virtual_character(character, input, dt));
    world.step(dt);
    state = world.virtual_character_state(character);
    REQUIRE(state.has_value());
    if (state) REQUIRE(state->linearVelocity.y > 1.0F);

    const auto snapshot = world.save_snapshot();
    REQUIRE(snapshot.has_value());
    const Float3 savedPosition = state ? state->transform.position : Float3{};
    for (int i = 0; i < 30; ++i) {
        input.jump = false;
        REQUIRE(world.update_virtual_character(character, input, dt));
        world.step(dt);
    }
    REQUIRE(snapshot && world.restore_snapshot(*snapshot));
    state = world.virtual_character_state(character);
    REQUIRE(state.has_value());
    if (state) REQUIRE(length(subtract(state->transform.position, savedPosition)) < 1.0e-4F);

    REQUIRE(world.destroy_virtual_character(character));
    REQUIRE(world.destroy_body(floor));
}
}

int main() {
    test_factory_configuration();
    test_mesh_queries_and_casts();
    test_snapshot_round_trip();
    test_virtual_character();
    if (failures == 0) {
        std::puts("dve_jolt_upstream_tests: PASS");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "dve_jolt_upstream_tests: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
