#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/ragdoll_runtime.hpp"
#if defined(DVE_V187_GAMEWORLD_INTEGRATION_TEST)
#include "dve/game_world.hpp"
#endif

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float epsilon = 0.001F) {
    return std::abs(a - b) <= epsilon;
}

SkeletonAsset skeleton_fixture() {
    SkeletonAsset skeleton;
    skeleton.name = "Recovery Skeleton";
    skeleton.bones = {
        {"pelvis", -1, make_rigid_transform({}, {})},
        {"spine", 0, make_rigid_transform({0.0F, 1.0F, 0.0F}, {})},
        {"head", 1, make_rigid_transform({0.0F, 1.0F, 0.0F}, {})},
    };
    return skeleton;
}

AnimationClipAsset clip(std::string name) {
    AnimationClipAsset result;
    result.name = std::move(name);
    result.durationSeconds = 1.0F;
    result.looping = false;
    return result;
}

RagdollDefinition ragdoll_fixture() {
    RagdollDefinition ragdoll;
    ragdoll.name = "Live Recovery Ragdoll";
    ragdoll.bodies = {{0U}, {1U}, {2U}};
    ragdoll.bodies[0].simulationWeight = 0.0F;
    ragdoll.joints = {
        {0U, 1U, RagdollJointKind::ConeTwist},
        {1U, 2U, RagdollJointKind::Fixed},
    };
    return ragdoll;
}

RigidBodyCreateDesc box_body(Float3 position) {
    RigidBodyCreateDesc desc;
    desc.transform = make_rigid_transform(position, {});
    desc.massKilograms = 1.0;
    desc.inertiaKilogramMetersSquared = {0.02, 0.02, 0.02, 0.0, 0.0, 0.0};
    desc.boxes.push_back({{}, {0.1F, 0.1F, 0.1F}});
    return desc;
}

void test_reference_constraint_lifecycle_and_solution() {
    ReferenceRigidBodyWorld world;
    world.set_gravity({});
    const RigidBodyHandle parent = world.create_body(box_body({0.0F, 0.0F, 0.0F}));
    const RigidBodyHandle child = world.create_body(box_body({1.0F, 0.0F, 0.0F}));
    require(parent != kInvalidRigidBodyHandle && child != kInvalidRigidBodyHandle,
            "reference constraint bodies were rejected");
    RigidBodyConstraintDesc desc;
    desc.parentBody = parent;
    desc.childBody = child;
    desc.kind = RigidBodyConstraintKind::Fixed;
    desc.parentAnchorLocal = {0.5F, 0.0F, 0.0F};
    desc.childAnchorLocal = {-0.5F, 0.0F, 0.0F};
    const RigidBodyConstraintHandle constraint = world.create_constraint(desc);
    require(constraint != kInvalidRigidBodyConstraintHandle && world.constraint_count() == 1U,
            "reference constraint was not created");

    RigidBodyState moved = *world.state(child);
    moved.currentTransform.position.x = 3.0F;
    moved.currentTransform.rotation = quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, 1.0F);
    require(world.set_state(child, moved), "could not perturb constrained child");
    world.step(1.0F / 60.0F);
    const auto parentState = world.state(parent);
    const auto childState = world.state(child);
    require(parentState && childState, "constrained body state disappeared");
    const Float3 parentAnchor = transform_point(parentState->currentTransform, desc.parentAnchorLocal);
    const Float3 childAnchor = transform_point(childState->currentTransform, desc.childAnchorLocal);
    require(length(subtract(parentAnchor, childAnchor)) < 0.001F,
            "live fixed constraint did not close its positional anchor");
    require(close(childState->currentTransform.rotation.w, parentState->currentTransform.rotation.w),
            "live fixed constraint did not restore reference rotation");
    require(world.destroy_body(parent), "constrained parent destruction failed");
    require(world.constraint_count() == 0U, "body destruction left a dangling live constraint");
}

void test_live_partial_ragdoll_settle_and_recovery() {
    SkeletalAnimationRuntime animation;
    std::string error;
    require(animation.bind_skeleton(7U, skeleton_fixture(), &error), error);
    require(animation.add_clip(7U, clip("Get Up Face Up"), &error), error);
    require(animation.add_clip(7U, clip("Get Up Face Down"), &error), error);
    require(animation.set_playback_speed(7U, 1.5F, &error), error);
    ReferenceRigidBodyWorld physics;
    physics.set_gravity({});
    RagdollRuntime runtime(animation, physics);
    RagdollRuntimeConfig config;
    config.blendInSeconds = 0.1F;
    config.settleSeconds = 0.2F;
    require(runtime.bind(7U, ragdoll_fixture(), config, &error), error);
    require(runtime.activate(7U, {}, {}, &error), error);
    require(runtime.body_handles(7U).size() == 3U && runtime.constraint_handles(7U).size() == 2U &&
            physics.constraint_count() == 2U, "activation did not publish a complete live ragdoll");

    const auto handles = runtime.body_handles(7U);
    RigidBodyState spine = *physics.state(handles[1]);
    spine.currentTransform.position.y += 0.4F;
    require(physics.set_state(handles[1], spine), "could not perturb partial ragdoll body");
    require(runtime.tick(7U, 0.1F, {}, &error), error);
    const LocalPose* pose = animation.local_pose(7U);
    require(pose && close((*pose)[0].position.y, 0.0F) && (*pose)[1].position.y > 1.3F,
            "per-body simulation weights did not preserve the animated pelvis and simulate the spine");

    for (int tick = 0; tick < 3; ++tick)
        require(runtime.tick(7U, 0.1F, {}, &error), error);
    require(runtime.settled(7U), "quiet live ragdoll did not reach deterministic settled state");

    RigidBodyState pelvis = *physics.state(handles[0]);
    pelvis.currentTransform.position = {5.0F, 0.0F, 0.0F};
    require(physics.set_state(handles[0], pelvis), "could not establish recovery root pose");
    RagdollRecoveryOptions recovery;
    recovery.faceUpClip = "Get Up Face Up";
    recovery.faceDownClip = "Get Up Face Down";
    recovery.poseBlendSeconds = 0.2F;
    RigidTransform aligned;
    require(runtime.begin_recovery(7U, {}, recovery, &aligned, &error), error);
    require(close(aligned.position.x, 5.0F) &&
            runtime.recovery_facing(7U) == RagdollRecoveryFacing::FaceUp,
            "recovery did not align the actor root or select face-up orientation");
    require(animation.active_clip(7U) == "Get Up Face Up",
            "orientation-specific face-up get-up clip was not selected");
    require(runtime.body_handles(7U).empty() && runtime.constraint_handles(7U).empty() &&
            physics.body_count() == 0U && physics.constraint_count() == 0U,
            "recovery did not release live ragdoll physics ownership");
    require(runtime.tick(7U, 0.1F, aligned, &error), error);
    require(runtime.state(7U) == RagdollRuntimeState::Recovering,
            "recovery blend completed too early");
    require(runtime.tick(7U, 0.11F, aligned, &error), error);
    require(runtime.state(7U) == RagdollRuntimeState::Animated && !runtime.owns_pose(7U),
            "recovery did not hand pose ownership back to animation");
    require(animation.playback_speed(7U) && close(*animation.playback_speed(7U), 1.5F),
            "recovery did not restore the actor's prior animation playback speed");
}

void test_face_down_clip_selection_and_invalid_weight() {
    SkeletonAsset skeleton = skeleton_fixture();
    RagdollDefinition invalid = ragdoll_fixture();
    invalid.bodies[1].simulationWeight = 1.1F;
    require(!validate_ragdoll(skeleton, invalid), "out-of-range partial ragdoll weight was accepted");
    invalid = ragdoll_fixture();
    invalid.joints[0].swingLimitRadians = 4.0F;
    require(!validate_ragdoll(skeleton, invalid), "out-of-range swing limit was accepted");
    invalid = ragdoll_fixture();
    invalid.joints[0].minimumRadians = -4.0F;
    require(!validate_ragdoll(skeleton, invalid), "out-of-range twist limit was accepted");

    SkeletalAnimationRuntime animation;
    std::string error;
    require(animation.bind_skeleton(9U, skeleton, &error), error);
    require(animation.add_clip(9U, clip("Up"), &error), error);
    require(animation.add_clip(9U, clip("Down"), &error), error);
    ReferenceRigidBodyWorld physics;
    physics.set_gravity({});
    RagdollRuntime runtime(animation, physics);
    require(runtime.bind(9U, ragdoll_fixture(), {}, &error), error);
    require(runtime.activate(9U, {}, {}, &error), error);
    const RigidBodyHandle pelvisHandle = runtime.body_handles(9U).front();
    RigidBodyState pelvis = *physics.state(pelvisHandle);
    pelvis.currentTransform.rotation = quaternion_from_axis_angle({1.0F, 0.0F, 0.0F}, 3.1415927F);
    require(physics.set_state(pelvisHandle, pelvis), "could not rotate face-down recovery body");
    RagdollRecoveryOptions recovery;
    recovery.faceUpClip = "Up";
    recovery.faceDownClip = "Down";
    RigidTransform aligned;
    require(runtime.begin_recovery(9U, {}, recovery, &aligned, &error), error);
    require(runtime.recovery_facing(9U) == RagdollRecoveryFacing::FaceDown &&
            animation.active_clip(9U) == "Down",
            "face-down pose did not select the face-down get-up clip");
}

#if defined(DVE_V187_GAMEWORLD_INTEGRATION_TEST)
void test_game_world_lifecycle_and_update_order() {
    auto physics = std::make_unique<ReferenceRigidBodyWorld>();
    GameWorld world(std::move(physics));
    GameObjectDesc actor;
    actor.name = "Ragdoll Actor";
    actor.transform = make_rigid_transform({2.0F, 0.0F, 0.0F}, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(actor), &error);
    require(id != kInvalidGameObjectId, error);
    require(world.animation().bind_skeleton(id, skeleton_fixture(), &error), error);
    require(world.animation().add_clip(id, clip("World Get Up"), &error), error);
    require(world.bind_ragdoll(id, ragdoll_fixture(), {}, &error), error);
    require(world.activate_ragdoll(id, {}, &error), error);
    world.tick(1.0F / 60.0F);
    require(world.ragdolls().owns_pose(id) && world.ragdolls().body_handles(id).size() == 3U,
            "GameWorld did not update the live ragdoll after physics and animation");
    RagdollRecoveryOptions recovery;
    recovery.getUpClip = "World Get Up";
    recovery.poseBlendSeconds = 0.05F;
    require(world.recover_ragdoll(id, recovery, &error), error);
    world.tick(0.06F);
    require(world.ragdolls().state(id) == RagdollRuntimeState::Animated,
            "GameWorld recovery did not return pose ownership to animation");
    require(world.destroy_object(id) && !world.ragdolls().has_instance(id),
            "GameWorld destruction did not release its ragdoll instance");
}
#endif

} // namespace

int main() {
    try {
        test_reference_constraint_lifecycle_and_solution();
        test_live_partial_ragdoll_settle_and_recovery();
        test_face_down_clip_selection_and_invalid_weight();
#if defined(DVE_V187_GAMEWORLD_INTEGRATION_TEST)
        test_game_world_lifecycle_and_update_order();
#endif
        std::cout << "dve_v187_ragdoll_runtime_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v187_ragdoll_runtime_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
