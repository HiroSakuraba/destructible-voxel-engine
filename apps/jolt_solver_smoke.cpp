// Validates JoltRigidBodyWorld against real narrow-phase contact resolution: a compound
// fragment body free-falls onto a static floor, must come to rest at the correct height,
// must eventually sleep, and must respond correctly to set_state() and interpolation.
// This is an adapter/integration smoke test, not a physics-quality benchmark.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dve/physics_jolt_backend.hpp"
#include "dve/ragdoll_runtime.hpp"
#include "dve/transform.hpp"

namespace {

int failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                           \
        }                                                                         \
    } while (false)

// Torque-free free flight must conserve angular momentum in the world frame:
//   L_world(t) = R(t) * I_body * R(t)^T * omega_world(t)
// This is the check the physics review specifically asked for: a box-drop test with a
// diagonal, axis-aligned inertia tensor cannot catch a row/column transposition or a sign
// error in an off-diagonal term, because a diagonal tensor is symmetric under those mistakes.
// A body with a genuine product-of-inertia term, given an initial spin that is not aligned
// with a principal axis, will visibly gain or lose angular momentum over time if the tensor
// was transposed or a sign was flipped when it was packed into Jolt's Mat44.
bool test_angular_momentum_conservation() {
    using namespace dve;
    const int startFailures = failures;

    JoltRigidBodyWorld world;
    world.set_gravity({0.0F, -9.81F, 0.0F});

    // Two 0.2 m half-extent, 5 kg boxes offset to (+-0.5, +-0.5, 0) from the shared center of
    // mass. Self-inertia of a solid box with half-extents h is m/3 * (h_a^2 + h_b^2) per axis
    // pair; parallel-axis adds m * d_a * d_b for the offset. Working this out by hand for the
    // two-box layout below gives a tensor with a genuine, nonzero Ixy term and Ixz = Iyz = 0,
    // which is enough to catch a transposed or sign-flipped xy entry while keeping the
    // arithmetic checkable by inspection.
    constexpr float kHalf = 0.2F;
    constexpr float kBoxMass = 5.0F;
    constexpr float kOffset = 0.5F;
    const double selfDiag = static_cast<double>(kBoxMass) / 3.0 * (2.0 * kHalf * kHalf);
    const double parallelSame = static_cast<double>(kBoxMass) * kOffset * kOffset; // per box, per axis pair sharing the offset axis
    const double ixx = 2.0 * selfDiag + 2.0 * parallelSame; // both boxes offset in x and y, none in z
    const double iyy = ixx;                                 // symmetric layout
    const double izz = 2.0 * selfDiag + 2.0 * 2.0 * parallelSame; // z-pair uses both x and y offsets
    const double ixy = -2.0 * static_cast<double>(kBoxMass) * kOffset * kOffset; // both boxes contribute the same sign
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform({0.0F, 50.0F, 0.0F}, {}); // far above the floor; must never contact anything
    desc.massKilograms = 2.0 * kBoxMass;
    desc.inertiaKilogramMetersSquared = {ixx, iyy, izz, ixy, 0.0, 0.0};
    desc.boxes = {
        SolverBox{{kOffset, kOffset, 0.0F}, {kHalf, kHalf, kHalf}},
        SolverBox{{-kOffset, -kOffset, 0.0F}, {kHalf, kHalf, kHalf}},
    };
    desc.angularVelocity = {2.0F, 0.7F, -1.3F}; // deliberately not aligned with any principal axis
    CHECK(inertia_is_positive_semidefinite(desc.inertiaKilogramMetersSquared));

    const RigidBodyHandle body = world.create_body(desc);
    CHECK(body != kInvalidRigidBodyHandle);
    // Jolt's default 0.05/s linear+angular damping is a deliberate applied torque/force, not
    // noise: leaving it on would make this specific conservation check fail for a reason
    // unrelated to whether the mass/inertia handoff is correct. Zero it just for this test.
    CHECK(world.set_damping(body, 0.0F, 0.0F));

    // Body-frame inertia matrix (constant) and a tiny rotation-matrix-from-quaternion helper,
    // both local to this test: this is a diagnostic, not something the adapter itself needs.
    const double bodyInertia[3][3] = {
        {ixx, ixy, 0.0},
        {ixy, iyy, 0.0},
        {0.0, 0.0, izz},
    };
    auto rotationColumns = [](Quaternion q) {
        return std::array<Float3, 3>{rotate(q, Float3{1.0F, 0.0F, 0.0F}), rotate(q, Float3{0.0F, 1.0F, 0.0F}),
                                      rotate(q, Float3{0.0F, 0.0F, 1.0F})};
    };
    auto worldAngularMomentum = [&](Quaternion rotation, Float3 omega) {
        const std::array<Float3, 3> r = rotationColumns(rotation); // columns of R
        // I_world = R * I_body * R^T, then L_world = I_world * omega.
        double rMat[3][3] = {
            {r[0].x, r[1].x, r[2].x},
            {r[0].y, r[1].y, r[2].y},
            {r[0].z, r[1].z, r[2].z},
        };
        double temp[3][3]{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) temp[i][j] += rMat[i][k] * bodyInertia[k][j];
        double iWorld[3][3]{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) iWorld[i][j] += temp[i][k] * rMat[j][k]; // R^T contribution
        const double omegaArr[3] = {omega.x, omega.y, omega.z};
        double l[3]{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) l[i] += iWorld[i][j] * omegaArr[j];
        return Float3{static_cast<float>(l[0]), static_cast<float>(l[1]), static_cast<float>(l[2])};
    };

    const auto initial = world.state(body);
    CHECK(initial.has_value());
    if (!initial.has_value()) return failures == startFailures;
    const Float3 lInitial = worldAngularMomentum(initial->currentTransform.rotation, initial->angularVelocity);
    const float lInitialMagnitude = length(lInitial);
    CHECK(lInitialMagnitude > 0.5F); // sanity: the test is actually exercising rotation

    constexpr float kFixedDelta = 1.0F / 120.0F;
    float worstRelativeError = 0.0F;
    for (int step = 0; step < 60; ++step) { // 0.5 s of pure free flight, well clear of the floor
        world.step(kFixedDelta);
        const auto current = world.state(body);
        CHECK(current.has_value());
        if (!current.has_value()) break;
        CHECK(current->currentTransform.position.y > 30.0F); // never should have reached the floor
        const Float3 lNow = worldAngularMomentum(current->currentTransform.rotation, current->angularVelocity);
        const float relativeError = length(subtract(lNow, lInitial)) / lInitialMagnitude;
        worstRelativeError = std::max(worstRelativeError, relativeError);
    }
    // 1% is generous for a 0.5 s / 60-step float-precision integration; a transposed or
    // sign-flipped tensor entry produces drift orders of magnitude larger than this, not a
    // marginal miss, so this tolerance is not fine-tuned to the bug it is meant to catch.
    CHECK(worstRelativeError < 0.01F);
    std::printf("angular_momentum_worst_relative_error=%.6f\n", static_cast<double>(worstRelativeError));

    CHECK(world.destroy_body(body));
    return failures == startFailures;
}


// Regression for the center-of-mass frame mismatch that occurs when equal-volume collision
// boxes represent unequal material masses. The two box centers are chosen so masses 1 kg and
// 4 kg have an authoritative physical COM at x=0, while Jolt's geometric compound COM would
// be x=-0.3 m if left uncorrected.
bool test_heterogeneous_center_of_mass_frame() {
    using namespace dve;
    const int startFailures = failures;

    JoltWorldConfig config;
    config.workerThreads = 1;
    JoltRigidBodyWorld world(config);
    world.set_gravity({});

    constexpr double leftMass = 1.0;
    constexpr double rightMass = 4.0;
    constexpr double side = 0.2;
    constexpr double selfPerMass = side * side / 6.0;
    constexpr double totalSelf = (leftMass + rightMass) * selfPerMass;

    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform({7.0F, 5.0F, -2.0F}, {});
    desc.massKilograms = leftMass + rightMass;
    desc.boxes = {
        SolverBox{{-0.8F, 0.0F, 0.0F}, {0.1F, 0.1F, 0.1F}},
        SolverBox{{0.2F, 0.0F, 0.0F}, {0.1F, 0.1F, 0.1F}},
    };
    desc.inertiaKilogramMetersSquared = {
        totalSelf,
        totalSelf + leftMass * 0.8 * 0.8 + rightMass * 0.2 * 0.2,
        totalSelf + leftMass * 0.8 * 0.8 + rightMass * 0.2 * 0.2,
        0.0, 0.0, 0.0,
    };
    CHECK(validate_rigid_body_desc(desc));

    const RigidBodyHandle body = world.create_body(desc);
    CHECK(body != kInvalidRigidBodyHandle);
    const auto shapeCom = world.solver_shape_center_of_mass_local(body);
    CHECK(shapeCom.has_value());
    if (shapeCom.has_value()) CHECK(length(*shapeCom) < 1.0e-5F);

    const auto bounds = world.solver_world_bounds(body);
    CHECK(bounds.has_value());
    if (bounds.has_value()) {
        CHECK(std::fabs(bounds->minimum.x - 6.1F) < 1.0e-4F);
        CHECK(std::fabs(bounds->maximum.x - 7.3F) < 1.0e-4F);
        CHECK(std::fabs(bounds->minimum.y - 4.9F) < 1.0e-4F);
        CHECK(std::fabs(bounds->maximum.y - 5.1F) < 1.0e-4F);
    }

    world.step(1.0F / 120.0F);
    CHECK(world.last_update_succeeded());
    const auto after = world.state(body);
    CHECK(after.has_value());
    if (after.has_value()) {
        CHECK(std::fabs(after->currentTransform.position.x - 7.0F) < 1.0e-5F);
        CHECK(std::fabs(after->currentTransform.position.y - 5.0F) < 1.0e-5F);
    }

    // An impulse away from the COM must produce angular motion while preserving the COM frame.
    CHECK(world.apply_impulse_at_point(body, {1.0F, 0.0F, 0.0F}, {7.0F, 6.0F, -2.0F}));
    const auto impulsed = world.state(body);
    CHECK(impulsed.has_value());
    if (impulsed.has_value()) CHECK(std::fabs(impulsed->angularVelocity.z) > 1.0e-4F);

    const JoltPhysicsTelemetry telemetry = world.telemetry();
    CHECK(telemetry.failedUpdateCalls == 0U);
    CHECK(telemetry.configuredWorkerThreads == 1U);
    CHECK(world.destroy_body(body));
    return failures == startFailures;
}

// A wall-collapse-shaped scenario: many fragments created in one call. Validates that
// create_bodies() pairs each returned handle with the correct descriptor even though Jolt's
// AddBodiesPrepare is documented to reorder the body-ID array in place for broad-phase
// locality, and that all bodies actually simulate (fall) rather than only the first/last.
bool test_batch_creation() {
    using namespace dve;
    const int startFailures = failures;

    JoltRigidBodyWorld world;
    world.set_gravity({0.0F, -9.81F, 0.0F});
    // Half-extent 100 m in x: with kFragmentCount fragments spaced 1.5 m apart starting at
    // x=0, the last one sits at x=58.5 m, which must still land on the floor rather than
    // falling past its edge into the void.
    const RigidBodyHandle floor = world.create_static_box(make_rigid_transform({0.0F, -1.0F, 0.0F}, {}), {100.0F, 1.0F, 50.0F});
    CHECK(floor != kInvalidRigidBodyHandle);

    constexpr int kFragmentCount = 40;
    std::vector<RigidBodyCreateDesc> descs;
    descs.reserve(kFragmentCount);
    for (int i = 0; i < kFragmentCount; ++i) {
        RigidBodyCreateDesc desc;
        // Distinct starting heights so a positional mix-up between handle and descriptor
        // would show up as bodies resting at the wrong height rather than all looking alike.
        desc.transform = make_rigid_transform({static_cast<float>(i) * 1.5F, 3.0F + static_cast<float>(i) * 0.1F, 0.0F}, {});
        desc.massKilograms = 2.0;
        desc.inertiaKilogramMetersSquared = {2.0 / 6.0, 2.0 / 6.0, 2.0 / 6.0, 0.0, 0.0, 0.0}; // solid 0.5m cube, mass 2 kg
        desc.boxes = {SolverBox{{0.0F, 0.0F, 0.0F}, {0.25F, 0.25F, 0.25F}}};
        descs.push_back(desc);
    }

    const std::vector<RigidBodyHandle> handles = world.create_bodies(descs);
    CHECK(handles.size() == descs.size());
    for (RigidBodyHandle handle : handles) CHECK(handle != kInvalidRigidBodyHandle);

    constexpr float kFixedDelta = 1.0F / 120.0F;
    const auto timingStart = std::chrono::steady_clock::now();
    constexpr int kSettleSteps = 300;
    for (int step = 0; step < kSettleSteps; ++step) world.step(kFixedDelta); // 2.5 s: enough to settle
    const auto timingEnd = std::chrono::steady_clock::now();
    const double averageStepMicroseconds =
        std::chrono::duration<double, std::micro>(timingEnd - timingStart).count() / static_cast<double>(kSettleSteps);
    // Not a hardware-representative measurement (this container, not the reference PC; only
    // 40 bodies, well under the 128-awake-body target in the physics review). Reported only
    // as a rough same-machine scaling sanity check, not a performance claim.
    std::printf(
        "batch_creation_average_step_us=%.2f (n=%d bodies, this-container-only, not a hardware benchmark)\n",
        averageStepMicroseconds, kFragmentCount);

    int restingCount = 0;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const auto s = world.state(handles[i]);
        CHECK(s.has_value());
        if (!s.has_value()) continue;
        // Each fragment must settle near its own starting x, not some other fragment's: this
        // is exactly what would break if the BodyID/descriptor pairing were wrong after the
        // AddBodiesPrepare reorder.
        const float expectedX = static_cast<float>(i) * 1.5F;
        CHECK(std::fabs(s->currentTransform.position.x - expectedX) < 0.05F);
        if (std::fabs(s->currentTransform.position.y - 0.25F) < 0.05F) ++restingCount;
    }
    CHECK(restingCount == kFragmentCount);

    for (RigidBodyHandle handle : handles) CHECK(world.destroy_body(handle));
    CHECK(world.destroy_body(floor));
    return failures == startFailures;
}

dve::RigidBodyCreateDesc constraint_box(
    dve::Float3 position, dve::Quaternion rotation = {}) {
    dve::RigidBodyCreateDesc desc;
    desc.transform = dve::make_rigid_transform(position, rotation);
    desc.massKilograms = 1.0;
    desc.inertiaKilogramMetersSquared = {0.02, 0.02, 0.02, 0.0, 0.0, 0.0};
    desc.boxes.push_back({{}, {0.1F, 0.1F, 0.1F}});
    desc.collisionClass = dve::RigidBodyCollisionClass::DebrisNoSelf;
    return desc;
}

bool test_live_constraints() {
    using namespace dve;
    const int startFailures = failures;
    JoltWorldConfig config;
    config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    world.set_gravity({});

    const std::array<RigidBodyConstraintKind, 4> kinds = {
        RigidBodyConstraintKind::Ball,
        RigidBodyConstraintKind::Hinge,
        RigidBodyConstraintKind::ConeTwist,
        RigidBodyConstraintKind::Fixed,
    };
    for (const RigidBodyConstraintKind kind : kinds) {
        const RigidBodyHandle parent = world.create_body(constraint_box({0.0F, 5.0F, 0.0F}));
        const RigidBodyHandle child = world.create_body(constraint_box({1.0F, 5.0F, 0.0F}));
        CHECK(parent != kInvalidRigidBodyHandle);
        CHECK(child != kInvalidRigidBodyHandle);
        RigidBodyConstraintDesc desc;
        desc.parentBody = parent;
        desc.childBody = child;
        desc.kind = kind;
        desc.parentAnchorLocal = {0.5F, 0.0F, 0.0F};
        desc.childAnchorLocal = {-0.5F, 0.0F, 0.0F};
        const RigidBodyConstraintHandle constraint = world.create_constraint(desc);
        CHECK(constraint != kInvalidRigidBodyConstraintHandle);
        CHECK(world.constraint_count() == 1U);
        world.step(1.0F / 120.0F);
        CHECK(world.last_update_succeeded());
        CHECK(world.destroy_constraint(constraint));
        CHECK(world.constraint_count() == 0U);
        CHECK(world.destroy_body(child));
        CHECK(world.destroy_body(parent));
    }

    const Quaternion reference = quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, 0.4F);
    const RigidBodyHandle parent = world.create_body(constraint_box({0.0F, 5.0F, 0.0F}));
    const RigidBodyHandle child = world.create_body(constraint_box({1.0F, 5.0F, 0.0F}, reference));
    RigidBodyConstraintDesc fixed;
    fixed.parentBody = parent;
    fixed.childBody = child;
    fixed.kind = RigidBodyConstraintKind::Fixed;
    fixed.parentAnchorLocal = {0.5F, 0.0F, 0.0F};
    fixed.childAnchorLocal = rotate(conjugate(reference), {-0.5F, 0.0F, 0.0F});
    fixed.referenceRotation = reference;
    const RigidBodyConstraintHandle fixedHandle = world.create_constraint(fixed);
    CHECK(fixedHandle != kInvalidRigidBodyConstraintHandle);

    RigidBodyState perturbed = *world.state(child);
    perturbed.currentTransform.position.x += 1.0F;
    perturbed.currentTransform.rotation = quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, 1.0F);
    perturbed.sleeping = false;
    CHECK(world.set_state(child, perturbed));
    for (int step = 0; step < 120; ++step) world.step(1.0F / 120.0F);
    const auto parentState = world.state(parent);
    const auto childState = world.state(child);
    CHECK(parentState.has_value());
    CHECK(childState.has_value());
    if (parentState && childState) {
        const Float3 parentAnchor = transform_point(
            parentState->currentTransform, fixed.parentAnchorLocal);
        const Float3 childAnchor = transform_point(
            childState->currentTransform, fixed.childAnchorLocal);
        CHECK(length(subtract(parentAnchor, childAnchor)) < 0.01F);
        const Quaternion relative = normalize(multiply(
            conjugate(parentState->currentTransform.rotation),
            childState->currentTransform.rotation));
        const float alignment = std::abs(relative.x * reference.x + relative.y * reference.y +
            relative.z * reference.z + relative.w * reference.w);
        CHECK(alignment > 0.99F);
    }
    CHECK(world.destroy_body(parent));
    CHECK(world.constraint_count() == 0U);
    CHECK(world.destroy_body(child));
    return failures == startFailures;
}

bool test_live_ragdoll_activation() {
    using namespace dve;
    const int startFailures = failures;
    SkeletalAnimationRuntime animation;
    SkeletonAsset skeleton;
    skeleton.name = "Jolt smoke skeleton";
    skeleton.bones = {
        {"pelvis", -1, make_rigid_transform({}, {})},
        {"spine", 0, make_rigid_transform({0.0F, 0.4F, 0.0F}, {})},
        {"head", 1, make_rigid_transform({0.0F, 0.35F, 0.0F}, {})},
    };
    std::string error;
    CHECK(animation.bind_skeleton(1U, skeleton, &error));
    AnimationClipAsset getUp;
    getUp.name = "Get Up";
    getUp.durationSeconds = 1.0F;
    getUp.looping = false;
    CHECK(animation.add_clip(1U, getUp, &error));

    RagdollDefinition definition;
    definition.name = "Jolt smoke ragdoll";
    definition.bodies = {{0U}, {1U}, {2U}};
    definition.joints = {
        {0U, 1U, RagdollJointKind::ConeTwist},
        {1U, 2U, RagdollJointKind::Hinge},
    };
    JoltWorldConfig config;
    config.workerThreads = 1U;
    JoltRigidBodyWorld world(config);
    world.set_gravity({0.0F, -9.81F, 0.0F});
    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -0.1F, 0.0F}, {}), {3.0F, 0.1F, 3.0F});
    CHECK(floor != kInvalidRigidBodyHandle);
    RagdollRuntime runtime(animation, world);
    RagdollRuntimeConfig ragdollConfig;
    ragdollConfig.blendInSeconds = 0.0F;
    ragdollConfig.settleLinearSpeed = 0.2F;
    ragdollConfig.settleAngularSpeed = 0.3F;
    ragdollConfig.settleSeconds = 0.25F;
    CHECK(runtime.bind(1U, definition, ragdollConfig, &error));
    const RigidTransform actorWorld = make_rigid_transform({0.0F, 2.0F, 0.0F}, {});
    CHECK(runtime.activate(1U, actorWorld, {}, &error));
    CHECK(runtime.body_handles(1U).size() == 3U);
    CHECK(runtime.constraint_handles(1U).size() == 2U);
    CHECK(world.body_count() == 4U);
    CHECK(world.constraint_count() == 2U);
    CHECK(runtime.apply_impulse(1U, 2U, {0.25F, 0.0F, 0.0F}));
    for (const RigidBodyHandle handle : runtime.body_handles(1U))
        CHECK(world.set_damping(handle, 0.4F, 0.4F));
    for (int step = 0; step < 1200 && !runtime.settled(1U); ++step) {
        world.step(1.0F / 120.0F);
        CHECK(runtime.tick(1U, 1.0F / 120.0F, actorWorld, &error));
    }
    CHECK(runtime.owns_pose(1U));
    CHECK(runtime.settled(1U));
    RagdollRecoveryOptions recovery;
    recovery.getUpClip = "Get Up";
    recovery.poseBlendSeconds = 0.05F;
    RigidTransform aligned;
    CHECK(runtime.begin_recovery(1U, actorWorld, recovery, &aligned, &error));
    CHECK(world.constraint_count() == 0U);
    CHECK(world.body_count() == 1U);
    CHECK(runtime.tick(1U, 0.06F, aligned, &error));
    CHECK(runtime.state(1U) == RagdollRuntimeState::Animated);
    CHECK(runtime.unbind(1U));
    CHECK(world.constraint_count() == 0U);
    CHECK(world.destroy_body(floor));
    CHECK(world.body_count() == 0U);
    return failures == startFailures;
}

} // namespace

int main() {
    using namespace dve;

    JoltRigidBodyWorld world;
    world.set_gravity({0.0F, -9.81F, 0.0F});

    // Static floor: top surface at y = 0.
    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -1.0F, 0.0F}, {}), {50.0F, 1.0F, 50.0F});
    CHECK(floor != kInvalidRigidBodyHandle);

    // Dynamic compound body: two 0.5 m half-extent boxes side by side, dropped from 3 m,
    // matching the shape of a two-brick fragment. Mass and inertia mirror
    // make_rigid_body_desc()'s output for a uniform-density solid box pair.
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform({0.0F, 3.0F, 0.0F}, {});
    desc.massKilograms = 20.0;
    // Solid-box inertia approximation for a combined 2m x 1m x 1m block, mass 20 kg:
    // I_xx = m/12 (h^2+d^2), etc. Values only need to be positive semidefinite and roughly
    // physical; exact correctness of this formula is validated separately in
    // rigid_body_adapter's own inertia tests.
    desc.inertiaKilogramMetersSquared = {
        20.0 / 12.0 * (1.0 * 1.0 + 1.0 * 1.0),
        20.0 / 12.0 * (2.0 * 2.0 + 1.0 * 1.0),
        20.0 / 12.0 * (2.0 * 2.0 + 1.0 * 1.0),
        0.0, 0.0, 0.0,
    };
    desc.boxes = {
        SolverBox{{-0.5F, 0.0F, 0.0F}, {0.5F, 0.5F, 0.5F}},
        SolverBox{{0.5F, 0.0F, 0.0F}, {0.5F, 0.5F, 0.5F}},
    };
    CHECK(inertia_is_positive_semidefinite(desc.inertiaKilogramMetersSquared));

    const RigidBodyHandle fragment = world.create_body(desc);
    CHECK(fragment != kInvalidRigidBodyHandle);

    constexpr float kFixedDelta = 1.0F / 120.0F;
    constexpr int kMaxSteps = 120 * 6; // up to 6 simulated seconds

    bool settled = false;
    bool sleptWithinBudget = false;
    int stepsToSleep = -1;
    for (int step = 0; step < kMaxSteps; ++step) {
        world.step(kFixedDelta);
        const auto activeNow = world.is_active(fragment);
        CHECK(activeNow.has_value());
        if (activeNow.has_value() && !*activeNow && stepsToSleep < 0) {
            stepsToSleep = step;
            sleptWithinBudget = true;
            break;
        }
    }
    CHECK(sleptWithinBudget);

    const auto finalState = world.state(fragment);
    CHECK(finalState.has_value());
    if (finalState.has_value()) {
        // Resting height: box bottoms (half extent 0.5) on a floor top at y = 0, so the
        // body's center of mass should settle near y = 0.5, well clear of falling through
        // (which would show as a large negative y) and of failing to make contact at all
        // (which would show as y staying near the 3 m drop height).
        const float restY = finalState->currentTransform.position.y;
        settled = std::fabs(restY - 0.5F) < 0.05F;
        CHECK(settled);
        std::printf("rest_y=%.4f steps_to_sleep=%d\n", static_cast<double>(restY), stepsToSleep);
    }

    // Complete state restoration preserves interpolation history and sleeping status.
    RigidBodyState restored;
    restored.previousTransform = make_rigid_transform({9.0F, 5.0F, 0.0F}, {});
    restored.currentTransform = make_rigid_transform({10.0F, 5.0F, 0.0F}, {});
    restored.sleeping = true;
    CHECK(world.set_state(fragment, restored));
    const auto restoredState = world.state(fragment);
    CHECK(restoredState.has_value());
    if (restoredState.has_value()) {
        CHECK(std::fabs(restoredState->previousTransform.position.x - 9.0F) < 1.0e-4F);
        CHECK(std::fabs(restoredState->currentTransform.position.x - 10.0F) < 1.0e-4F);
        CHECK(restoredState->sleeping);
    }

    // Teleport is a distinct operation that collapses history and wakes the body.
    CHECK(world.teleport_body(fragment, make_rigid_transform({10.0F, 5.0F, 0.0F}, {})));
    world.step(kFixedDelta);
    const auto afterTeleport = world.state(fragment);
    CHECK(afterTeleport.has_value());
    if (afterTeleport.has_value()) {
        CHECK(afterTeleport->currentTransform.position.x > 9.0F);
    }

    // Interpolation sanity: alpha 0 must equal previous, alpha 1 must equal current.
    if (afterTeleport.has_value()) {
        const RigidTransform atZero = world.interpolated_transform(fragment, 0.0F);
        const RigidTransform atOne = world.interpolated_transform(fragment, 1.0F);
        CHECK(std::fabs(atZero.position.x - afterTeleport->previousTransform.position.x) < 1.0e-4F);
        CHECK(std::fabs(atOne.position.x - afterTeleport->currentTransform.position.x) < 1.0e-4F);
    }

    const RigidBodyCounts preDestroyCounts = world.body_counts();
    CHECK(preDestroyCounts.total == 2U);
    CHECK(preDestroyCounts.staticBodies == 1U);
    CHECK(preDestroyCounts.dynamicBodies == 1U);
    CHECK(world.destroy_body(fragment));
    CHECK(world.destroy_body(floor));
    CHECK(world.body_count() == 0U);
    CHECK(world.active_body_count() == 0U);

    const bool momentumOk = test_angular_momentum_conservation();
    const bool centerOfMassOk = test_heterogeneous_center_of_mass_frame();
    const bool batchOk = test_batch_creation();
    const bool constraintsOk = test_live_constraints();
    const bool ragdollOk = test_live_ragdoll_activation();
    if (!momentumOk) std::fprintf(stderr, "test_angular_momentum_conservation: FAILED\n");
    if (!centerOfMassOk) std::fprintf(stderr, "test_heterogeneous_center_of_mass_frame: FAILED\n");
    if (!batchOk) std::fprintf(stderr, "test_batch_creation: FAILED\n");
    if (!constraintsOk) std::fprintf(stderr, "test_live_constraints: FAILED\n");
    if (!ragdollOk) std::fprintf(stderr, "test_live_ragdoll_activation: FAILED\n");

    if (failures == 0) {
        std::printf("dve_jolt_solver_smoke: PASS\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "dve_jolt_solver_smoke: %d FAILURE(S)\n", failures);
    return EXIT_FAILURE;
}
