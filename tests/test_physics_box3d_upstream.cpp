#include "dve/physics3d_backend.hpp"
#include "dve/physics_box3d_backend.hpp"

#include <box3d/box3d.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace dve;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "Box3D integration test failed: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

SolverInertiaTensor box_inertia(float mass, Float3 halfExtents) {
    const double hx = halfExtents.x;
    const double hy = halfExtents.y;
    const double hz = halfExtents.z;
    SolverInertiaTensor inertia;
    inertia.xx = static_cast<double>(mass) * (hy * hy + hz * hz) / 3.0;
    inertia.yy = static_cast<double>(mass) * (hx * hx + hz * hz) / 3.0;
    inertia.zz = static_cast<double>(mass) * (hx * hx + hy * hy) / 3.0;
    return inertia;
}

RigidBodyCreateDesc dynamic_box(Float3 position, Float3 halfExtents, float mass = 2.0F) {
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform(position, {});
    desc.massKilograms = mass;
    desc.inertiaKilogramMetersSquared = box_inertia(mass, halfExtents);
    desc.boxes.push_back({{}, halfExtents});
    desc.allowSleeping = true;
    return desc;
}

class RecordingSink final : public IPhysicsContactSink {
public:
    bool record_contact(const PhysicsContactEvent& event) noexcept override {
        events.push_back(event);
        return true;
    }

    void body_removed(std::uint64_t body) noexcept override {
        removed.push_back(body);
    }

    std::vector<PhysicsContactEvent> events;
    std::vector<std::uint64_t> removed;
};

void test_version_and_factory() {
    const b3Version version = b3GetVersion();
    require(version.major == 0 && version.minor == 1, "uploaded solver must be Box3D 0.1.x");

    const Physics3DBackendAvailability availability = physics3d_backend_availability();
    require(availability.reference, "reference backend must always be available");
    require(availability.box3d, "Box3D backend must be reported available");
    require(physics3d_backend_available(Physics3DBackend::Box3D), "Box3D availability query failed");
    require(physics3d_backend_label(Physics3DBackend::Box3D) == "Box3D", "Box3D label mismatch");

    Physics3DBackend resolved = Physics3DBackend::Reference;
    std::string error;
    std::unique_ptr<IRigidBodyWorld> world = create_physics3d_world(
        Physics3DBackend::Box3D, &resolved, &error);
    require(world != nullptr && error.empty(), "factory could not create Box3D backend");
    require(resolved == Physics3DBackend::Box3D, "factory resolved wrong backend");

    world = create_physics3d_world(Physics3DBackend::Automatic, &resolved, &error);
    require(world != nullptr && error.empty(), "automatic factory could not create a backend");
#ifdef DVE_HAVE_JOLT
    require(resolved == Physics3DBackend::Jolt, "automatic factory must preserve Jolt priority");
#else
    require(resolved == Physics3DBackend::Box3D, "automatic factory did not choose Box3D");
#endif

    world = create_physics3d_world(Physics3DBackend::Reference, &resolved, &error);
    require(world != nullptr && resolved == Physics3DBackend::Reference,
        "reference backend selection failed");
#ifndef DVE_HAVE_JOLT
    world = create_physics3d_world(Physics3DBackend::Jolt, &resolved, &error);
    require(world == nullptr && !error.empty(), "unavailable explicit Jolt selection did not fail");
#endif
}

void test_falling_body_contacts_and_materials() {
    Box3DWorldConfig config;
    config.workerThreads = 1;
    config.subStepCount = 4;
    config.hitEventThresholdMetersPerSecond = 0.05F;
    Box3DRigidBodyWorld world(config);
    RecordingSink sink;
    world.set_contact_sink(&sink);

    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -0.5F, 0.0F}, {}), {8.0F, 0.5F, 8.0F});
    require(floor != kInvalidRigidBodyHandle, "static floor creation failed");
    require(world.set_contact_material(floor, 11U), "static material update failed");

    RigidBodyCreateDesc desc = dynamic_box({0.0F, 3.0F, 0.0F}, {0.5F, 0.5F, 0.5F});
    desc.useContinuousCollision = true;
    const RigidBodyHandle body = world.create_body(desc);
    require(body != kInvalidRigidBodyHandle, "dynamic box creation failed");
    require(world.set_contact_material(body, 7U), "dynamic material update failed");

    for (int i = 0; i < 300; ++i) world.step(1.0F / 60.0F);
    const auto state = world.state(body);
    require(state.has_value(), "body state missing after stepping");
    require(state->currentTransform.position.y > 0.40F && state->currentTransform.position.y < 0.62F,
        "dynamic body did not settle on the floor");
    require(!sink.events.empty(), "Box3D hit events were not bridged to the contact sink");

    bool sawMaterials = false;
    for (const PhysicsContactEvent& event : sink.events) {
        const bool direct = event.materialA == 7U && event.materialB == 11U;
        const bool reverse = event.materialA == 11U && event.materialB == 7U;
        if (direct || reverse) {
            sawMaterials = true;
            require(event.effectiveMass > 0.0F, "contact effective mass was not computed");
            require(event.normalImpulse >= 0.0F, "contact impulse estimate was negative");
            break;
        }
    }
    require(sawMaterials, "contact material identities were not preserved");

    require(world.apply_impulse(body, {0.0F, 6.0F, 0.0F}), "center impulse failed");
    world.step(1.0F / 60.0F);
    require(world.state(body)->linearVelocity.y > 0.0F, "impulse did not affect velocity");
    require(world.apply_force(body, {2.0F, 0.0F, 0.0F}), "center force failed");
    require(world.apply_impulse_at_point(body, {0.0F, 0.0F, 1.0F}, {0.5F, 0.5F, 0.0F}),
        "point impulse failed");
    require(world.set_damping(body, 0.1F, 0.2F), "damping update failed");

    const Box3DPhysicsTelemetry telemetry = world.telemetry();
    require(telemetry.versionMajor == 0U && telemetry.versionMinor == 1U,
        "telemetry version mismatch");
    require(telemetry.stepCalls == 301U, "telemetry step count mismatch");
    require(telemetry.contactHitEvents > 0U, "telemetry did not count contact hits");
    require(telemetry.contactEventsQueued == sink.events.size(), "queued contact metric mismatch");
    require(telemetry.bodyCounts.total == 2U, "body count telemetry mismatch");
    require(telemetry.box3dAllocatedBytes > 0U, "Box3D allocation telemetry missing");

    require(world.destroy_body(body), "dynamic body destruction failed");
    require(!sink.removed.empty() && sink.removed.back() == body,
        "body removal was not published to the contact sink");
    require(world.destroy_body(floor), "static body destruction failed");
}

void test_compound_mass_frame_and_state_restore() {
    Box3DRigidBodyWorld world;
    world.set_gravity({0.0F, 0.0F, 0.0F});

    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform({4.0F, 2.0F, -3.0F}, quaternion_from_euler_xyz({0.2F, -0.3F, 0.1F}));
    desc.massKilograms = 5.0;
    desc.inertiaKilogramMetersSquared = {2.0, 3.0, 4.0, 0.1, 0.05, -0.02};
    desc.boxes.push_back({{-0.75F, 0.0F, 0.0F}, {0.5F, 0.4F, 0.3F}});
    desc.boxes.push_back({{0.75F, 0.0F, 0.0F}, {0.5F, 0.4F, 0.3F}});
    desc.linearVelocity = {1.0F, 0.5F, -0.25F};
    desc.angularVelocity = {0.1F, 0.2F, 0.3F};

    const RigidBodyHandle body = world.create_body(desc);
    require(body != kInvalidRigidBodyHandle, "compound body creation failed");
    const auto localCenter = world.solver_shape_center_of_mass_local(body);
    require(localCenter.has_value(), "compound center inspection failed");
    require(near(localCenter->x, 0.0F, 1.0e-5F) && near(localCenter->y, 0.0F, 1.0e-5F) &&
            near(localCenter->z, 0.0F, 1.0e-5F),
        "Box3D body center does not match DVE authoritative COM frame");

    const auto bounds = world.solver_world_bounds(body);
    require(bounds.has_value(), "compound world bounds unavailable");
    require(bounds->maximum.x > bounds->minimum.x && bounds->maximum.y > bounds->minimum.y &&
            bounds->maximum.z > bounds->minimum.z,
        "compound world bounds invalid");

    world.step(1.0F / 60.0F);
    const RigidBodyState moved = *world.state(body);
    require(moved.currentTransform.position.x > desc.transform.position.x,
        "zero-gravity body did not integrate its velocity");
    const RigidTransform midpoint = world.interpolated_transform(body, 0.5F);
    require(midpoint.position.x >= moved.previousTransform.position.x &&
            midpoint.position.x <= moved.currentTransform.position.x,
        "interpolation did not stay between snapshots");

    RigidBodyState restored;
    restored.previousTransform = make_rigid_transform({1.0F, 2.0F, 3.0F}, {});
    restored.currentTransform = make_rigid_transform({2.0F, 3.0F, 4.0F}, {});
    restored.linearVelocity = {-1.0F, 0.0F, 0.0F};
    restored.angularVelocity = {};
    restored.sleeping = false;
    require(world.set_state(body, restored), "state restoration failed");
    const RigidBodyState checked = *world.state(body);
    require(near(checked.previousTransform.position.x, 1.0F, 1.0e-5F),
        "previous interpolation state was not restored");
    require(near(checked.currentTransform.position.x, 2.0F, 1.0e-5F),
        "current transform was not restored");
    require(near(checked.linearVelocity.x, -1.0F, 1.0e-5F),
        "linear velocity was not restored");

    require(world.teleport_body(body, make_rigid_transform({10.0F, 5.0F, 0.0F}, {})),
        "teleport failed");
    const RigidBodyState teleported = *world.state(body);
    require(near(teleported.previousTransform.position.x, 10.0F, 1.0e-5F) &&
            near(teleported.currentTransform.position.x, 10.0F, 1.0e-5F),
        "teleport did not collapse interpolation history");
}


void test_static_mesh_and_queries() {
    Box3DRigidBodyWorld world;
    const std::vector<Float3> vertices{
        {-2.0F, 0.0F, -2.0F},
        {2.0F, 0.0F, -2.0F},
        {2.0F, 0.0F, 2.0F},
        {-2.0F, 0.0F, 2.0F},
    };
    const std::vector<std::uint32_t> indices{0U, 2U, 1U, 0U, 3U, 2U};
    const RigidBodyHandle mesh = world.create_static_triangle_mesh(
        make_rigid_transform({0.0F, 0.0F, 0.0F}, {}), vertices, indices,
        {1.0F, 1.0F, 1.0F}, 23U);
    require(mesh != kInvalidRigidBodyHandle, "static triangle mesh creation failed");

    const auto ray = world.ray_cast_closest({0.0F, 3.0F, 0.0F}, {0.0F, -6.0F, 0.0F});
    require(ray.has_value(), "ray did not hit static mesh");
    require(ray->body == mesh, "ray resolved the wrong body");
    require(ray->material == 23U, "ray did not preserve mesh material");
    require(ray->triangleIndex >= 0, "mesh ray did not report a triangle index");
    require(ray->normal.y > 0.8F, "mesh ray normal was not upward");
    require(near(ray->point.y, 0.0F, 1.0e-4F), "mesh ray hit point was incorrect");

    std::vector<Box3DOverlapHit> overlaps(4U);
    const std::size_t overlapCount = world.query_aabb(
        {{-0.5F, -0.1F, -0.5F}, {0.5F, 0.1F, 0.5F}}, overlaps);
    require(overlapCount == 1U, "AABB query did not return exactly one mesh body");
    require(overlaps[0].body == mesh && overlaps[0].material == 23U,
        "AABB query did not preserve mesh body and material");

    const RigidBodyHandle body = world.create_body(
        dynamic_box({0.0F, 2.0F, 0.0F}, {0.25F, 0.25F, 0.25F}));
    require(body != kInvalidRigidBodyHandle, "dynamic body above mesh failed");
    for (int i = 0; i < 180; ++i) world.step(1.0F / 60.0F);
    const auto settled = world.state(body);
    require(settled.has_value() && settled->currentTransform.position.y > 0.20F &&
            settled->currentTransform.position.y < 0.35F,
        "dynamic body did not settle on the static mesh");

    require(world.destroy_body(mesh), "static mesh destruction failed");
    require(!world.ray_cast_closest({0.0F, 3.0F, 0.0F}, {0.0F, -2.0F, 0.0F}).has_value(),
        "destroyed mesh remained queryable");
}

void test_constraints_and_collision_classes() {
    Box3DRigidBodyWorld world;
    world.set_gravity({0.0F, 0.0F, 0.0F});

    const RigidBodyHandle parent = world.create_body(dynamic_box({0.0F, 0.0F, 0.0F}, {0.25F, 0.25F, 0.25F}));
    const RigidBodyHandle child = world.create_body(dynamic_box({0.0F, -1.0F, 0.0F}, {0.25F, 0.25F, 0.25F}));
    require(parent != kInvalidRigidBodyHandle && child != kInvalidRigidBodyHandle,
        "constraint test bodies failed");

    for (RigidBodyConstraintKind kind : {
             RigidBodyConstraintKind::Ball,
             RigidBodyConstraintKind::Hinge,
             RigidBodyConstraintKind::ConeTwist,
             RigidBodyConstraintKind::Fixed}) {
        RigidBodyConstraintDesc desc;
        desc.parentBody = parent;
        desc.childBody = child;
        desc.kind = kind;
        desc.parentAnchorLocal = {0.0F, -0.5F, 0.0F};
        desc.childAnchorLocal = {0.0F, 0.5F, 0.0F};
        desc.parentAxisLocal = {0.0F, 0.0F, 1.0F};
        desc.referenceRotation = {};
        desc.minimumRadians = -0.4F;
        desc.maximumRadians = 0.4F;
        desc.swingLimitRadians = 0.6F;
        const RigidBodyConstraintHandle constraint = world.create_constraint(desc);
        require(constraint != kInvalidRigidBodyConstraintHandle, "Box3D rejected a supported DVE constraint");
        require(world.constraint_count() == 1U, "constraint count did not increase");
        world.step(1.0F / 60.0F);
        require(world.destroy_constraint(constraint), "constraint destruction failed");
        require(world.constraint_count() == 0U, "constraint count did not decrease");
    }

    RigidBodyCreateDesc debrisA = dynamic_box({3.0F, 0.0F, 0.0F}, {0.5F, 0.5F, 0.5F});
    debrisA.collisionClass = RigidBodyCollisionClass::DebrisNoSelf;
    RigidBodyCreateDesc debrisB = debrisA;
    debrisB.transform.position = {3.25F, 0.0F, 0.0F};
    const RigidBodyHandle debrisHandleA = world.create_body(debrisA);
    const RigidBodyHandle debrisHandleB = world.create_body(debrisB);
    require(debrisHandleA != kInvalidRigidBodyHandle && debrisHandleB != kInvalidRigidBodyHandle,
        "debris bodies failed");
    world.step(1.0F / 60.0F);
    const auto debrisStateA = world.state(debrisHandleA);
    const auto debrisStateB = world.state(debrisHandleB);
    require(debrisStateA.has_value() && debrisStateB.has_value(), "debris states missing");
    require(near(debrisStateA->currentTransform.position.x, 3.0F, 0.02F) &&
            near(debrisStateB->currentTransform.position.x, 3.25F, 0.02F),
        "DebrisNoSelf bodies incorrectly pushed each other apart");

    require(world.destroy_body(parent), "destroying constrained parent failed");
    require(world.constraint_count() == 0U, "body destruction left stale constraints");
}

void test_solver_neutral_loads_and_queries() {
    Box3DRigidBodyWorld world;
    world.set_gravity({});
    IRigidBodyWorld& neutral = world;

    const RigidBodyHandle first = world.create_static_box(
        make_rigid_transform({}, {}), {0.5F, 0.5F, 0.5F});
    const RigidBodyHandle second = world.create_static_box(
        make_rigid_transform({3.0F, 0.0F, 0.0F}, {}), {0.5F, 0.5F, 0.5F});
    const RigidBodyHandle dynamic = world.create_body(
        dynamic_box({6.0F, 0.0F, 0.0F}, {0.25F, 0.25F, 0.25F}));
    require(first != kInvalidRigidBodyHandle && second != kInvalidRigidBodyHandle &&
            dynamic != kInvalidRigidBodyHandle,
        "neutral-query test body creation failed");
    require(neutral.set_contact_material(first, 11U) &&
            neutral.set_contact_material(second, 22U) &&
            neutral.set_contact_material(dynamic, 33U),
        "neutral-query materials failed");

    const auto rayHits = neutral.ray_cast_all(
        {-3.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, 12.0F);
    require(rayHits.size() == 3U, "neutral ray-all did not collect every body");
    require(rayHits[0].body == first && rayHits[0].material == 11U &&
            rayHits[1].body == second && rayHits[1].material == 22U &&
            rayHits[2].body == dynamic && rayHits[2].material == 33U,
        "neutral ray-all ordering or material metadata was incorrect");
    require(rayHits[0].distance < rayHits[1].distance &&
            rayHits[1].distance < rayHits[2].distance,
        "neutral ray-all distances were not sorted");

    RigidBodyQueryFilter dynamicOnly;
    dynamicOnly.includeStatic = false;
    const auto dynamicHits = neutral.ray_cast_all(
        {-3.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 12.0F, dynamicOnly);
    require(dynamicHits.size() == 1U && dynamicHits.front().body == dynamic,
        "neutral ray-all static/dynamic filter failed");
    RigidBodyQueryFilter ignored;
    ignored.ignoredBodies.push_back(first);
    const auto ignoredHits = neutral.ray_cast_all(
        {-3.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 12.0F, ignored);
    require(ignoredHits.size() == 2U && ignoredHits.front().body == second,
        "neutral ray-all ignored-body filter failed");

    const auto overlaps = neutral.overlap_aabb(
        {{-0.75F, -0.75F, -0.75F}, {0.75F, 0.75F, 0.75F}});
    require(overlaps.size() == 1U && overlaps.front().body == first &&
            overlaps.front().material == 11U,
        "neutral overlap did not preserve body and material metadata");
    const auto sphereHits = neutral.cast_sphere_all(
        {-3.0F, 0.0F, 0.0F}, 0.2F, {1.0F, 0.0F, 0.0F}, 12.0F);
    require(sphereHits.size() == 3U && sphereHits[0].body == first &&
            sphereHits[1].body == second && sphereHits[2].body == dynamic,
        "neutral sphere cast-all did not return stable ordered hits");

    require(neutral.apply_angular_impulse(dynamic, {0.0F, 0.0F, 1.0F}),
        "neutral angular impulse failed");
    const auto afterImpulse = neutral.state(dynamic);
    require(afterImpulse.has_value() && afterImpulse->angularVelocity.z > 0.0F,
        "neutral angular impulse did not change angular velocity");
    const float impulseVelocity = afterImpulse ? afterImpulse->angularVelocity.z : 0.0F;
    require(neutral.apply_torque(dynamic, {0.0F, 0.0F, 2.0F}),
        "neutral torque failed");
    world.step(1.0F / 60.0F);
    const auto afterTorque = neutral.state(dynamic);
    require(afterTorque.has_value() && afterTorque->angularVelocity.z > impulseVelocity,
        "neutral torque did not affect angular velocity");
    require(!neutral.apply_torque(first, {0.0F, 0.0F, 1.0F}),
        "neutral torque accepted a static body");
}

} // namespace

int main() {
    test_version_and_factory();
    test_falling_body_contacts_and_materials();
    test_compound_mass_frame_and_state_restore();
    test_static_mesh_and_queries();
    test_constraints_and_collision_classes();
    test_solver_neutral_loads_and_queries();
    std::cout << "Box3D upstream integration tests passed\n";
    return 0;
}
