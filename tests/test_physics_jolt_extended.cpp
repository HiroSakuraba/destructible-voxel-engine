#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "dve/jolt_replay.hpp"
#include "dve/physics_jolt_backend.hpp"
#include "dve/soft_body.hpp"
#include "dve/transform.hpp"

namespace {
int failures = 0;
#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (false)
using namespace dve;

RigidBodyCreateDesc make_box(Float3 position, Float3 halfExtents, double mass = 1000.0) {
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform(position, {});
    desc.massKilograms = mass;
    const double x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    desc.inertiaKilogramMetersSquared = {
        mass / 3.0 * (y * y + z * z), mass / 3.0 * (x * x + z * z),
        mass / 3.0 * (x * x + y * y), 0.0, 0.0, 0.0};
    desc.boxes.push_back({{}, halfExtents});
    return desc;
}

void test_height_field_and_mutable_compound() {
    JoltWorldConfig config; config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    constexpr std::uint32_t side = 8U;
    std::array<float, side * side> heights{};
    JoltHeightFieldDesc terrainDesc;
    terrainDesc.sampleCount = side;
    terrainDesc.sampleOffset = {-3.5F, 0.0F, -3.5F};
    terrainDesc.sampleScale = {1.0F, 1.0F, 1.0F};
    terrainDesc.material = 41U;
    const RigidBodyHandle terrain = world.create_static_height_field(terrainDesc, heights);
    REQUIRE(terrain != kInvalidRigidBodyHandle);
    auto ray = world.ray_cast_closest({0.0F, 4.0F, 0.0F}, {0.0F, -8.0F, 0.0F});
    REQUIRE(ray && ray->body == terrain && ray->material == 41U);

    const std::array<float, 1> peak{{2.0F}};
    REQUIRE(world.update_height_field_region(terrain, 4U, 4U, 1U, 1U, peak));
    ray = world.ray_cast_closest({0.5F, 4.0F, 0.5F}, {0.0F, -8.0F, 0.0F});
    REQUIRE(ray && ray->point.y > 1.5F);

    JoltMutableCompoundDesc compoundDesc;
    compoundDesc.transform = make_rigid_transform({0.0F, 1.0F, 4.0F}, {});
    compoundDesc.boxes = {{{-1.0F, 0.0F, 0.0F}, {}, {0.5F, 0.5F, 0.5F}, 1U},
                          {{ 1.0F, 0.0F, 0.0F}, {}, {0.5F, 0.5F, 0.5F}, 2U}};
    const RigidBodyHandle compound = world.create_mutable_compound(compoundDesc);
    REQUIRE(compound != kInvalidRigidBodyHandle);
    const JoltMutableCompoundChildHandle child = world.add_mutable_box(
        compound, {{0.0F, 1.0F, 0.0F}, {}, {0.25F, 0.25F, 0.25F}, 3U});
    REQUIRE(child != kInvalidJoltMutableCompoundChildHandle);
    REQUIRE(world.modify_mutable_box(compound, child,
        {{0.0F, 1.5F, 0.0F}, {}, {0.35F, 0.35F, 0.35F}, 3U}));
    REQUIRE(world.remove_mutable_box(compound, child));
    REQUIRE(world.telemetry().heightFieldUpdates == 1U);
    REQUIRE(world.telemetry().mutableCompoundEdits == 3U);
}

void test_soft_bodies_hair_and_replay() {
    JoltWorldConfig config; config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -0.5F, 0.0F}, {}), {10.0F, 0.5F, 10.0F});
    REQUIRE(floor != kInvalidRigidBodyHandle);

    SoftBodyRopeRecipe ropeRecipe;
    ropeRecipe.segments = 8U;
    ropeRecipe.spacing = 0.2F;
    SoftBodyAsset rope = make_soft_body_rope(ropeRecipe);
    rope.bendModel = SoftBodyBendModel::CosseratRod;
    JoltSoftBodyDesc softDesc;
    softDesc.transform = make_rigid_transform({0.0F, 3.0F, 0.0F}, {});
    softDesc.vertexRadiusMeters = 0.03F;
    const JoltSoftBodyHandle soft = world.create_soft_body(rope, softDesc);
    REQUIRE(soft != kInvalidJoltSoftBodyHandle);
    auto softState = world.soft_body_state(soft);
    REQUIRE(softState && softState->vertices.size() == rope.vertices.size());
    REQUIRE(world.apply_soft_body_impulse(soft, {1.0F, 0.0F, 0.0F}));
    REQUIRE(world.set_soft_body_vertex_inverse_mass(soft, 0U, 0.0F));
    REQUIRE(world.set_soft_body_vertex_velocity(soft, 1U, {0.5F, 0.0F, 0.0F}));
    softState = world.soft_body_state(soft);
    REQUIRE(softState && std::abs(softState->vertices[1].velocity.x - 0.5F) < 1.0e-3F);

    JoltHairStrandDesc strand;
    strand.points = {{1.0F, 3.0F, 0.0F}, {1.0F, 2.8F, 0.0F}, {1.0F, 2.6F, 0.0F}, {1.0F, 2.4F, 0.0F}};
    const std::array<JoltHairStrandDesc, 1> strands{{strand}};
    const JoltSoftBodyHandle hair = world.create_hair_rods(strands, {});
    REQUIRE(hair != kInvalidJoltSoftBodyHandle);

    for (int i = 0; i < 20; ++i) world.step(1.0F / 120.0F);
    softState = world.soft_body_state(soft);
    REQUIRE(softState && std::isfinite(softState->vertices[1].position.x) &&
            std::abs(softState->vertices[1].position.x) > 1.0e-4F);

    JoltReplayTrack replay;
    REQUIRE(replay.capture(world, 20U));
    for (int i = 0; i < 10; ++i) world.step(1.0F / 120.0F);
    REQUIRE(replay.capture(world, 30U));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "dve_jolt_extended_test.dvejoltreplay";
    std::string error;
    REQUIRE(replay.write(path, &error));
    const JoltReplayReadResult read = JoltReplayTrack::read(path);
    REQUIRE(read && read.frames.size() == 2U && read.frames[1].simulationFrame == 30U);
    REQUIRE(world.restore_snapshot(read.frames[0].snapshot));
    std::error_code ec; std::filesystem::remove(path, ec);

    REQUIRE(world.destroy_soft_body(hair));
    REQUIRE(world.destroy_soft_body(soft));
    REQUIRE(world.destroy_body(floor));
}

void test_vehicle_and_debug_capture() {
    JoltWorldConfig config; config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -0.5F, 0.0F}, {}), {30.0F, 0.5F, 30.0F});
    REQUIRE(floor != kInvalidRigidBodyHandle);

    JoltWheeledVehicleDesc vehicleDesc;
    vehicleDesc.chassis = make_box({0.0F, 1.2F, 0.0F}, {0.9F, 0.25F, 1.6F}, 900.0);
    const std::array<Float3, 4> positions{{{-0.75F, -0.15F, 1.1F}, {0.75F, -0.15F, 1.1F},
                                           {-0.75F, -0.15F, -1.1F}, {0.75F, -0.15F, -1.1F}}};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        JoltVehicleWheelDesc wheel;
        wheel.position = positions[i];
        wheel.maximumSteerAngleRadians = i < 2U ? 0.45F : 0.0F;
        wheel.maximumHandBrakeTorque = i >= 2U ? 4000.0F : 0.0F;
        vehicleDesc.wheels.push_back(wheel);
    }
    vehicleDesc.differentials = {{0, 1, 0.5F, 1.4F}, {2, 3, 0.5F, 1.4F}};
    const JoltVehicleHandle vehicle = world.create_wheeled_vehicle(vehicleDesc);
    REQUIRE(vehicle != kInvalidJoltVehicleHandle);
    REQUIRE(world.set_vehicle_input(vehicle, {0.5F, 0.2F, 0.0F, 0.0F}));
    for (int i = 0; i < 120; ++i) world.step(1.0F / 120.0F);
    const auto state = world.vehicle_state(vehicle);
    REQUIRE(state && state->wheels.size() == 4U);
    bool anyContact = false;
    if (state) for (const auto& wheel : state->wheels) anyContact = anyContact || wheel.hasContact;
    REQUIRE(anyContact);

    const JoltDebugFrame debug = world.capture_debug_frame();
    REQUIRE(debug.supported);
    REQUIRE(!debug.lines.empty() || !debug.triangles.empty());
    REQUIRE(world.telemetry().vehicleUpdates == 1U);
    REQUIRE(world.telemetry().debugFramesCaptured == 1U);
    REQUIRE(world.destroy_vehicle(vehicle));
    REQUIRE(world.destroy_body(floor));
}
}

int main() {
    test_height_field_and_mutable_compound();
    test_soft_bodies_hair_and_replay();
    test_vehicle_and_debug_capture();
    if (failures == 0) { std::puts("dve_jolt_extended_tests: PASS"); return EXIT_SUCCESS; }
    std::fprintf(stderr, "dve_jolt_extended_tests: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
