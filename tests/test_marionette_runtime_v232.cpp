#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/marionette_runtime.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float tolerance = 1.0e-3F) {
    return std::abs(left - right) <= tolerance;
}

SkeletonAsset make_skeleton() {
    SkeletonAsset skeleton;
    skeleton.name = "Articulated Marionette Test Skeleton";
    skeleton.bones = {
        {"pelvis", -1, make_rigid_transform({0.0F, 1.0F, 0.0F}, {})},       // 0
        {"spine", 0, make_rigid_transform({0.0F, 0.25F, 0.0F}, {})},        // 1
        {"chest", 1, make_rigid_transform({0.0F, 0.28F, 0.0F}, {})},        // 2
        {"neck", 2, make_rigid_transform({0.0F, 0.20F, 0.0F}, {})},         // 3
        {"head", 3, make_rigid_transform({0.0F, 0.18F, 0.0F}, {})},         // 4
        {"left_upper_arm", 2, make_rigid_transform({-0.22F, 0.12F, 0.0F}, {})}, // 5
        {"left_lower_arm", 5, make_rigid_transform({-0.30F, 0.0F, 0.0F}, {})},  // 6
        {"left_hand", 6, make_rigid_transform({-0.27F, 0.0F, 0.0F}, {})},       // 7
        {"right_upper_arm", 2, make_rigid_transform({0.22F, 0.12F, 0.0F}, {})}, // 8
        {"right_lower_arm", 8, make_rigid_transform({0.30F, 0.0F, 0.0F}, {})},  // 9
        {"right_hand", 9, make_rigid_transform({0.27F, 0.0F, 0.0F}, {})},       // 10
        {"left_thigh", 0, make_rigid_transform({-0.12F, -0.18F, 0.0F}, {})},   // 11
        {"left_calf", 11, make_rigid_transform({0.0F, -0.42F, 0.0F}, {})},      // 12
        {"left_foot", 12, make_rigid_transform({0.0F, -0.38F, 0.10F}, {})},     // 13
        {"right_thigh", 0, make_rigid_transform({0.12F, -0.18F, 0.0F}, {})},   // 14
        {"right_calf", 14, make_rigid_transform({0.0F, -0.42F, 0.0F}, {})},     // 15
        {"right_foot", 15, make_rigid_transform({0.0F, -0.38F, 0.10F}, {})},    // 16
    };
    skeleton.contentHash = skeleton_content_hash(skeleton);
    return skeleton;
}

HumanoidMarionetteBones make_mapping() {
    HumanoidMarionetteBones bones;
    bones.pelvis = 0U;
    bones.spine = 1U;
    bones.chest = 2U;
    bones.neck = 3U;
    bones.head = 4U;
    bones.leftUpperArm = 5U;
    bones.leftLowerArm = 6U;
    bones.leftHand = 7U;
    bones.rightUpperArm = 8U;
    bones.rightLowerArm = 9U;
    bones.rightHand = 10U;
    bones.leftThigh = 11U;
    bones.leftCalf = 12U;
    bones.leftFoot = 13U;
    bones.rightThigh = 14U;
    bones.rightCalf = 15U;
    bones.rightFoot = 16U;
    return bones;
}

void test_setup_generation() {
    const SkeletonAsset skeleton = make_skeleton();
    const HumanoidMarionetteBones mapping = make_mapping();
    require(static_cast<bool>(validate_humanoid_marionette_bones(skeleton, mapping)),
            "valid humanoid marionette mapping was rejected");
    HumanoidMarionetteSetup setup;
    std::string error;
    require(build_humanoid_marionette_setup(skeleton, mapping, setup, &error), error);
    require(setup.controlRig.controls.size() == 11U, "marionette control count is wrong");
    require(setup.controlRig.nodes.size() == 7U, "marionette solve-node count is wrong");
    require(setup.ragdoll.bodies.size() == 15U && setup.ragdoll.joints.size() == 14U,
            "fully articulated ragdoll was not generated");
    require(static_cast<bool>(validate_marionette_asset(setup.controlRig, setup.ragdoll, setup.marionette)),
            "generated marionette asset failed validation");
    require(setup.marionette.contentHash == marionette_content_hash(setup.marionette),
            "marionette content hash is not deterministic");
}

void test_gamepad_and_vr_input() {
    MarionetteInputRouter router;
    router.reset(make_rigid_transform({0.0F, 2.5F, 0.0F}, {}));
    platform::PlatformEvent event;
    event.type = platform::EventType::GamepadAxisMotion;
    event.gamepadAxis = platform::GamepadAxis::LeftX;
    event.gamepadValue = 1.0F;
    router.consume_gamepad_event(event);
    event.gamepadAxis = platform::GamepadAxis::RightTrigger;
    router.consume_gamepad_event(event);
    event.type = platform::EventType::GamepadButtonDown;
    event.gamepadButton = platform::GamepadButton::West;
    router.consume_gamepad_event(event);
    event.gamepadButton = platform::GamepadButton::North;
    router.consume_gamepad_event(event);
    MarionetteInputFrame frame = router.sample(0.1F, {});
    require(frame.crossbarWorld.position.x > 0.1F && frame.crossbarWorld.position.y > 2.5F,
            "gamepad axes did not move the crossbar");
    require(frame.stringPullMeters[static_cast<std::size_t>(MarionetteStringRole::LeftHand)] > 0.5F,
            "gamepad face button did not pull the left-hand string");
    require(frame.requestModeCycle, "gamepad mode-cycle pulse was not emitted");
    require(!router.sample(0.0F, {}).requestModeCycle, "gamepad pulse repeated without another event");

    MarionetteVrInput vr;
    vr.enabled = true;
    vr.left.tracked = true;
    vr.right.tracked = true;
    vr.left.world.position = {-0.4F, 2.4F, 0.1F};
    vr.right.world.position = {0.4F, 2.4F, 0.1F};
    vr.left.trigger = 1.0F;
    vr.right.grip = 1.0F;
    router.set_vr_input(vr);
    frame = router.sample(1.0F / 90.0F, {});
    require(frame.vrTracked && close(frame.crossbarWorld.position.y, 2.4F),
            "tracked VR controllers did not define the crossbar");
    require(frame.stringPullMeters[static_cast<std::size_t>(MarionetteStringRole::LeftHand)] > 0.5F,
            "VR trigger did not pull the hand string");
    require(frame.tensionScale > 0.9F, "VR grip did not adjust string tension");
}

void test_assisted_and_physical_runtime() {
    const SkeletonAsset skeleton = make_skeleton();
    HumanoidMarionetteSetup setup;
    std::string error;
    require(build_humanoid_marionette_setup(skeleton, make_mapping(), setup, &error), error);

    SkeletalAnimationRuntime animation;
    require(animation.bind_skeleton(77U, skeleton, &error), error);
    ControlRigRuntime controlRig(animation);
    require(controlRig.bind(77U, setup.controlRig, &error), error);
    ReferenceRigidBodyWorld physics;
    physics.set_gravity({});
    RagdollRuntime ragdolls(animation, physics);
    require(ragdolls.bind(77U, setup.ragdoll, {}, &error), error);

    MarionetteRuntime runtime(animation, controlRig, ragdolls, physics);
    MarionetteRuntimeConfig config;
    config.initialMode = MarionetteControlMode::AssistedRig;
    require(runtime.bind(77U, setup.marionette, config, &error), error);

    MarionetteInputFrame input;
    input.crossbarWorld = make_rigid_transform({0.0F, 2.8F, 0.0F}, {});
    input.stringPullMeters[static_cast<std::size_t>(MarionetteStringRole::LeftHand)] = 0.25F;
    input.bodyLeanLocal = {0.1F, 0.0F, 0.0F};
    require(runtime.tick(77U, 1.0F / 60.0F, {}, input, &error), error);
    const MarionetteTelemetry* telemetry = runtime.telemetry(77U);
    require(telemetry && telemetry->rigEvaluated && !telemetry->ragdollActive,
            "assisted mode did not evaluate the control rig");
    require(animation.local_pose(77U) && animation.local_pose(77U)->size() == skeleton.bones.size(),
            "assisted marionette did not publish a skeletal pose");

    require(runtime.set_mode(77U, MarionetteControlMode::Physical, {}, &error), error);
    require(ragdolls.owns_pose(77U) && ragdolls.body_handles(77U).size() == setup.ragdoll.bodies.size(),
            "physical mode did not activate the full ragdoll");
    const std::uint16_t headBody = setup.marionette.strings[
        static_cast<std::size_t>(MarionetteStringRole::Head)].ragdollBody;
    const RigidBodyHandle headHandle = ragdolls.body_handles(77U)[headBody];
    RigidBodyState headState = *physics.state(headHandle);
    headState.currentTransform.position.y -= 1.5F;
    require(physics.set_state(headHandle, headState), "could not stretch the head string");
    require(runtime.tick(77U, 1.0F / 60.0F, {}, input, &error), error);
    telemetry = runtime.telemetry(77U);
    const auto& headTelemetry = telemetry->strings[static_cast<std::size_t>(MarionetteStringRole::Head)];
    require(telemetry->ragdollActive && headTelemetry.bodyAvailable && headTelemetry.forceNewtons > 0.0F,
            "physical string did not apply a spring force to the stretched head body");
    physics.step(1.0F / 60.0F);
    const auto movedHead = physics.state(headHandle);
    require(movedHead && movedHead->linearVelocity.y > 0.0F,
            "marionette string force did not accelerate the body toward the crossbar");
}

} // namespace

int main() {
    try {
        test_setup_generation();
        test_gamepad_and_vr_input();
        test_assisted_and_physical_runtime();
        std::cout << "dve_v232_marionette_runtime_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v232_marionette_runtime_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
