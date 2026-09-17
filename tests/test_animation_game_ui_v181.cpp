#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "dve/animation_controller.hpp"
#include "dve/game_ui.hpp"
#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 2.0e-3F) {
    return std::abs(a - b) <= tolerance;
}

SkeletonAsset make_chain() {
    SkeletonAsset skeleton;
    skeleton.name = "Controller IK Rig";
    skeleton.bones = {
        {"root", -1, make_rigid_transform({}, {})},
        {"upper", 0, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
        {"lower", 1, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
    };
    return skeleton;
}

AnimationClipAsset make_idle() {
    AnimationClipAsset clip;
    clip.name = "Idle";
    clip.durationSeconds = 1.0F;
    clip.looping = true;
    return clip;
}

AnimationClipAsset make_walk() {
    AnimationClipAsset clip;
    clip.name = "Walk";
    clip.durationSeconds = 1.0F;
    clip.looping = true;
    BoneAnimationTrack root;
    root.bone = 0U;
    root.translations = {{0.0F, {}}, {1.0F, {1.0F, 0.0F, 0.0F}}};
    clip.tracks.push_back(std::move(root));
    return clip;
}

AnimationControllerAsset make_controller() {
    AnimationControllerAsset controller;
    controller.name = "Locomotion";
    controller.initialState = "idle";
    controller.parameters.emplace("speed", 0.0);
    controller.states = {
        {"idle", "Idle", 1.0F, false},
        {"walk", "Walk", 1.0F, true},
    };
    controller.transitions = {
        {"idle", "walk", 0.1F, 0.0F, 10, {{"speed", AnimationConditionOperator::Greater, 0.5}}},
        {"walk", "idle", 0.1F, 0.0F, 10, {{"speed", AnimationConditionOperator::LessOrEqual, 0.5}}},
    };
    return controller;
}

void test_controller_and_root_motion() {
    const SkeletonAsset skeleton = make_chain();
    AnimationControllerAsset controller = make_controller();
    require(static_cast<bool>(validate_animation_controller(controller)), "valid controller was rejected");
    AnimationControllerAsset invalid = controller;
    invalid.initialState = "missing";
    require(!validate_animation_controller(invalid), "missing controller state was accepted");

    SkeletalAnimationRuntime animation;
    AnimationControllerRuntime runtime(animation);
    std::string error;
    require(animation.bind_skeleton(7U, skeleton, &error), error);
    require(animation.add_clip(7U, make_idle(), &error), error);
    require(animation.add_clip(7U, make_walk(), &error), error);
    require(runtime.bind(7U, controller, &error), error);
    require(runtime.state(7U) == "idle", "controller did not enter initial state");
    require(!runtime.set_parameter(7U, "speed", std::int64_t{1}, &error),
            "controller accepted a parameter type mismatch");
    require(runtime.set_parameter(7U, "speed", 1.0, &error), error);
    runtime.tick(0.5F);
    animation.tick(0.5F);
    require(runtime.state(7U) == "walk", "controller condition did not transition to walk");
    const auto motion = animation.consume_root_motion(7U);
    require(motion && close(motion->translation.x, 0.5F), "walk root motion was not extracted");
    animation.tick(0.75F);
    const auto loopMotion = animation.consume_root_motion(7U);
    require(loopMotion && close(loopMotion->translation.x, 0.75F),
            "root motion was not preserved across a looping clip boundary");
    const LocalPose* pose = animation.local_pose(7U);
    require(pose && close(pose->front().position.x, 0.0F), "extracted root motion remained in the pose");

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Root Motion Actor";
    const GameObjectId actor = world.create_object(std::move(desc), &error);
    require(actor != kInvalidGameObjectId, error);
    require(world.animation().bind_skeleton(actor, skeleton, &error), error);
    require(world.animation().add_clip(actor, make_idle(), &error), error);
    require(world.animation().add_clip(actor, make_walk(), &error), error);
    require(world.animation_controllers().bind(actor, make_controller(), &error), error);
    require(world.animation_controllers().set_parameter(actor, "speed", 1.0, &error), error);
    world.tick(0.5F);
    const auto transformed = world.transform(actor);
    require(transformed && close(transformed->position.x, 0.5F),
            "GameWorld did not apply extracted root motion to the actor");
}

void test_ik_and_ragdoll_bridge() {
    const SkeletonAsset skeleton = make_chain();
    LocalPose pose = make_bind_pose(skeleton);
    TwoBoneIkRequest request;
    request.root = 0U;
    request.middle = 1U;
    request.end = 2U;
    request.targetModel = {1.2F, 1.2F, 0.0F};
    request.poleModel = {0.0F, 0.0F, 1.0F};
    std::string error;
    require(solve_two_bone_ik(skeleton, pose, request, &error), error);
    const auto solved = compute_model_pose(skeleton, pose, &error);
    require(solved.size() == 3U && close(solved[2].position.x, request.targetModel.x, 1.0e-2F) &&
            close(solved[2].position.y, request.targetModel.y, 1.0e-2F),
            "two-bone IK endpoint did not reach its target");
    request.middle = 2U;
    require(!solve_two_bone_ik(skeleton, pose, request, &error), "invalid IK chain was accepted");

    RagdollDefinition ragdoll;
    ragdoll.name = "Reference Ragdoll";
    ragdoll.bodies = {{0U}, {1U}, {2U}};
    ragdoll.joints = {{0U, 1U}, {1U, 2U}};
    require(static_cast<bool>(validate_ragdoll(skeleton, ragdoll)), "valid ragdoll recipe was rejected");
    RagdollDefinition invalid = ragdoll;
    invalid.joints[0].childBody = 0U;
    require(!validate_ragdoll(skeleton, invalid), "self-connected ragdoll joint was accepted");

    const LocalPose animationPose = make_bind_pose(skeleton);
    const RigidTransform objectWorld = make_rigid_transform({10.0F, 0.0F, 0.0F}, {});
    std::vector<RigidTransform> bodyWorld = {
        make_rigid_transform({10.0F, 1.0F, 0.0F}, {}),
        make_rigid_transform({11.0F, 1.0F, 0.0F}, {}),
        make_rigid_transform({12.0F, 1.0F, 0.0F}, {}),
    };
    const LocalPose simulated = ragdoll_pose_from_body_world(
        skeleton, ragdoll, objectWorld, bodyWorld, animationPose, 1.0F, &error);
    const auto simulatedModel = compute_model_pose(skeleton, simulated, &error);
    require(simulatedModel.size() == 3U && close(simulatedModel[0].position.y, 1.0F) &&
            close(simulatedModel[2].position.x, 2.0F), "ragdoll body-to-pose bridge is incorrect");
    RagdollBlendState blend;
    blend.set_simulated(true);
    blend.tick(0.125F);
    require(close(blend.weight, 0.5F), "ragdoll blend state did not advance deterministically");
}

void test_gameplay_ui_runtime() {
    using namespace dve::ui;
    UiRuntime runtime;
    std::string error;
    const UiCanvasId canvas = runtime.create_canvas({"HUD", UiCanvasMode::ScreenSpace, {800.0F, 600.0F}}, &error);
    require(canvas != kInvalidUiCanvasId, error);
    UiDocument* document = runtime.document(canvas);
    require(document != nullptr, "created UI canvas has no document");
    UiWidget* root = document->widget(document->root());
    root->layout.direction = UiLayoutDirection::Vertical;
    root->layout.padding = {10.0F, 10.0F, 10.0F, 10.0F};
    root->layout.spacing = 5.0F;
    const UiWidgetId label = document->add_widget(document->root(), UiWidgetKind::Text, "Status", &error);
    require(label != kInvalidUiWidgetId, error);
    UiWidget* labelWidget = document->widget(label);
    labelWidget->layout.preferredSize = {200.0F, 30.0F};
    labelWidget->binding = "status";
    labelWidget->bindingTarget = UiBindingTarget::Text;
    const UiWidgetId slider = document->add_widget(document->root(), UiWidgetKind::Slider, "Volume", &error);
    require(slider != kInvalidUiWidgetId, error);
    UiWidget* sliderWidget = document->widget(slider);
    sliderWidget->layout.preferredSize = {200.0F, 40.0F};
    sliderWidget->focusable = true;
    sliderWidget->step = 0.25;
    const UiWidgetId button = document->add_widget(document->root(), UiWidgetKind::Button, "Confirm", &error);
    require(button != kInvalidUiWidgetId, error);
    UiWidget* buttonWidget = document->widget(button);
    buttonWidget->layout.preferredSize = {200.0F, 40.0F};
    buttonWidget->focusable = true;
    buttonWidget->binding = "confirm.enabled";
    buttonWidget->bindingTarget = UiBindingTarget::Enabled;
    require(document->validate(&error), error);
    runtime.data().set("status", std::string("Ready"));
    runtime.data().set("confirm.enabled", false);
    runtime.set_accessibility({1.5F, false, true, true});
    runtime.rebuild();
    const auto& commands = runtime.draw_commands();
    require(commands.size() == 3U && commands[0].text == "Ready", "UI data binding did not reach draw commands");
    require(close(commands[0].rectangle.x, 10.0F) && close(commands[0].rectangle.y, 10.0F) &&
            close(commands[0].textScale, 1.5F), "UI layout or accessibility scale is incorrect");
    require(runtime.navigate(canvas, UiNavigation::Next) && runtime.focused_widget(canvas) == slider,
            "UI navigation did not acquire first focusable widget");
    require(runtime.navigate(canvas, UiNavigation::Right), "UI slider navigation failed");
    auto events = runtime.take_events();
    require(events.size() == 1U && events[0].action == "change" &&
            std::get<double>(events[0].value) == 0.25, "UI slider event payload is incorrect");
    require(runtime.navigate(canvas, UiNavigation::Next) && runtime.focused_widget(canvas) == slider,
            "UI navigation focused a data-bound disabled widget");
    runtime.data().set("confirm.enabled", true);
    require(runtime.navigate(canvas, UiNavigation::Next) && runtime.focused_widget(canvas) == button,
            "UI navigation did not advance to a newly enabled widget");
    require(runtime.navigate(canvas, UiNavigation::Activate), "UI activation failed");
    events = runtime.take_events();
    require(events.size() == 1U && events[0].action == "activate", "UI activation event was not emitted");

    const UiCanvasId worldCanvas = runtime.create_canvas(
        {"World Prompt", UiCanvasMode::WorldSpace, {200.0F, 80.0F},
         make_rigid_transform({2.0F, 3.0F, 4.0F}, {}), 0.002F, 2, true}, &error);
    require(worldCanvas != kInvalidUiCanvasId, error);
    UiDocument* worldDocument = runtime.document(worldCanvas);
    require(worldDocument->add_widget(worldDocument->root(), UiWidgetKind::Text, "Marker", &error) !=
            kInvalidUiWidgetId, error);
    runtime.rebuild();
    const auto& worldCommands = runtime.draw_commands();
    const auto foundWorld = std::find_if(worldCommands.begin(), worldCommands.end(), [worldCanvas](const auto& command) {
        return command.canvas == worldCanvas;
    });
    require(foundWorld != worldCommands.end() && foundWorld->canvasMode == UiCanvasMode::WorldSpace &&
            close(foundWorld->worldTransform.position.z, 4.0F), "world-space UI contract lost its transform");

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    const UiCanvasId gameCanvas = world.ui_runtime().create_canvas({"Game HUD"}, &error);
    require(gameCanvas != kInvalidUiCanvasId, error);
    *world.ui_runtime().document(gameCanvas) = make_default_gameplay_hud();
    world.ui_runtime().data().set("player.health", 0.75);
    world.ui_runtime().data().set("hud.interaction", std::string("Open door"));
    world.tick(1.0F / 60.0F);
    require(world.ui_runtime().draw_commands().size() == 2U,
            "GameWorld did not rebuild the gameplay HUD during its fixed update");
}

} // namespace

int main() {
    try {
        test_controller_and_root_motion();
        test_ik_and_ragdoll_bridge();
        test_gameplay_ui_runtime();
        std::cout << "DVE v1.81 animation controller and gameplay UI tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.81 animation controller and gameplay UI tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
