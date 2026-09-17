#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include "dve/control_rig.hpp"
#include "dve/component.hpp"
#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 1.0e-2F) {
    return std::abs(a - b) <= tolerance;
}

SkeletonAsset make_chain() {
    SkeletonAsset skeleton;
    skeleton.name = "Control Rig Test Skeleton";
    skeleton.bones = {
        {"root", -1, make_rigid_transform({}, {})},
        {"upper", 0, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
        {"lower", 1, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
        {"hand", 2, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
    };
    skeleton.sockets = {{"grip", 3U, {}}};
    return skeleton;
}

ControlRigControl model_control(ControlRigControlId id, std::string name, Float3 position) {
    ControlRigControl control;
    control.id = id;
    control.name = std::move(name);
    control.defaultLocal.position = position;
    return control;
}

ControlRigNode set_node(
    ControlRigNodeId id, std::string name, BoneIndex bone, ControlRigControlId control,
    ControlRigSolvePhase phase = ControlRigSolvePhase::ForwardSolve) {
    ControlRigNode node;
    node.id = id;
    node.name = std::move(name);
    node.kind = ControlRigNodeKind::SetBoneTransform;
    node.phase = phase;
    node.bone = bone;
    node.targetControl = control;
    return node;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void test_validation_spaces_limits_and_phases() {
    const SkeletonAsset skeleton = make_chain();
    ControlRigAsset rig;
    rig.name = "Spaces and Phases";
    ControlRigControl limited = model_control(1U, "Limited", {1.0F, 0.0F, 0.0F});
    limited.limits.translationEnabled = true;
    limited.limits.minimumTranslation = {-2.0F, -2.0F, -2.0F};
    limited.limits.maximumTranslation = {2.0F, 2.0F, 2.0F};
    ControlRigControl boneSpace = model_control(2U, "Bone Space", {0.25F, 0.0F, 0.0F});
    boneSpace.space = ControlRigSpace::Bone;
    boneSpace.spaceBone = 2U;
    ControlRigControl childSpace = model_control(3U, "Control Child", {0.5F, 0.0F, 0.0F});
    childSpace.space = ControlRigSpace::Control;
    childSpace.parentControl = 1U;
    rig.controls = {limited, boneSpace, childSpace, model_control(4U, "Post", {2.0F, 0.0F, 0.0F})};
    // Authored in reverse phase order: stable phase scheduling still runs Pre before Post.
    rig.nodes = {
        set_node(1U, "Post Set", 0U, 4U, ControlRigSolvePhase::PostSolve),
        set_node(2U, "Pre Set", 0U, 1U, ControlRigSolvePhase::PreSolve),
    };
    require(static_cast<bool>(validate_control_rig(skeleton, rig)), "valid control rig was rejected");

    ControlRigAsset invalid = rig;
    invalid.controls[0].space = ControlRigSpace::Control;
    invalid.controls[0].parentControl = 3U;
    invalid.controls[2].parentControl = 1U;
    require(!validate_control_rig(skeleton, invalid), "cyclic control spaces were accepted");

    SkeletalAnimationRuntime animation;
    ControlRigRuntime runtime(animation);
    std::string error;
    require(animation.bind_skeleton(9U, skeleton, &error), error);
    require(runtime.bind(9U, rig, &error), error);
    require(runtime.set_control_local(9U, 1U, make_rigid_transform({20.0F, 0.0F, 0.0F}, {}), &error), error);
    const RigidTransform* clamped = runtime.control_local(9U, 1U);
    require(clamped && close(clamped->position.x, 2.0F), "control translation limit was not enforced");
    require(runtime.evaluate(9U, &error), error);
    const LocalPose* pose = animation.local_pose(9U);
    require(pose && close((*pose)[0].position.x, 2.0F), "solve phases did not execute in phase order");
    const RigidTransform* childModel = runtime.control_model(9U, 3U);
    require(childModel && close(childModel->position.x, 2.5F), "control-parent space did not resolve");
    const RigidTransform* boneModel = runtime.control_model(9U, 2U);
    require(boneModel && close(boneModel->position.x, 2.25F), "bone space did not resolve from the input pose");
    require(runtime.reset_control(9U, 1U, &error), error);
    require(runtime.control_local(9U, 1U) && close(runtime.control_local(9U, 1U)->position.x, 1.0F),
            "control reset did not restore the authored default");

    const auto root = std::filesystem::temp_directory_path() / "dve_control_rig_v182_assets";
    std::error_code filesystemError;
    std::filesystem::remove_all(root, filesystemError);
    const auto pathA = root / "spaces_a.dverig";
    const auto pathB = root / "spaces_b.dverig";
    require(write_dvecontrolrig(pathA, skeleton, rig, &error), error);
    const ControlRigReadResult loaded = read_dvecontrolrig(pathA);
    require(static_cast<bool>(loaded), loaded.error);
    require(loaded.asset.contentHash == control_rig_content_hash(rig), "control rig content hash changed on read");
    require(write_dvecontrolrig(pathB, skeleton, loaded.asset, &error), error);
    require(read_bytes(pathA) == read_bytes(pathB), "control rig serialization is not byte deterministic");
    {
        std::ofstream stream(pathB, std::ios::binary | std::ios::app);
        stream << "junk";
    }
    require(!read_dvecontrolrig(pathB), "control rig reader accepted trailing data");
    const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    const ComponentTypeSchema* schema = registry.find("dve.control_rig");
    require(schema && !schema->allowMultiple && schema->properties.size() == 3U &&
            schema->properties.front().assetReference, "control rig component schema is missing");
    ControlRigAsset stale = rig;
    stale.contentHash = 1U;
    require(!runtime.bind(9U, stale, &error), "runtime accepted a stale control rig content hash");
    std::filesystem::remove_all(root, filesystemError);
}

void test_set_copy_and_aim_nodes() {
    const SkeletonAsset skeleton = make_chain();
    std::string error;

    ControlRigAsset setRig;
    setRig.name = "Set Copy";
    setRig.controls = {model_control(1U, "Root Target", {0.0F, 2.0F, 0.0F})};
    ControlRigNode set = set_node(1U, "Move Root", 0U, 1U);
    set.affectRotation = false;
    ControlRigNode copy;
    copy.id = 2U;
    copy.name = "Copy Root To Hand";
    copy.kind = ControlRigNodeKind::CopyBoneTransform;
    copy.phase = ControlRigSolvePhase::PostSolve;
    copy.bone = 3U;
    copy.sourceBone = 0U;
    copy.offset.position = {0.0F, 1.0F, 0.0F};
    setRig.nodes = {set, copy};
    LocalPose output;
    std::map<ControlRigControlId, RigidTransform> controls;
    require(evaluate_control_rig(skeleton, setRig, make_bind_pose(skeleton), controls, output, nullptr, &error), error);
    const auto model = compute_model_pose(skeleton, output, &error);
    require(model.size() == 4U && close(model[0].position.y, 2.0F) && close(model[3].position.y, 3.0F),
            "set/copy bone nodes produced the wrong model transforms");

    ControlRigAsset aimRig;
    aimRig.name = "Aim";
    aimRig.controls = {model_control(1U, "Aim Target", {0.0F, 3.0F, 0.0F})};
    ControlRigNode aim;
    aim.id = 1U;
    aim.name = "Aim Root";
    aim.kind = ControlRigNodeKind::AimConstraint;
    aim.bone = 0U;
    aim.targetControl = 1U;
    aimRig.nodes = {aim};
    require(evaluate_control_rig(skeleton, aimRig, make_bind_pose(skeleton), controls, output, nullptr, &error), error);
    const auto aimed = compute_model_pose(skeleton, output, &error);
    require(aimed.size() == 4U && close(aimed[1].position.x, 0.0F) && close(aimed[1].position.y, 1.0F),
            "aim constraint did not rotate the authored aim axis toward its control");
}

void test_ik_and_fabrik_nodes() {
    const SkeletonAsset skeleton = make_chain();
    std::string error;
    std::map<ControlRigControlId, RigidTransform> values;
    LocalPose output;

    ControlRigAsset ikRig;
    ikRig.name = "Control Two Bone IK";
    ikRig.controls = {
        model_control(1U, "Hand Target", {2.0F, 1.0F, 0.0F}),
        model_control(2U, "Elbow Pole", {0.0F, 0.0F, 1.0F}),
    };
    ControlRigNode ik;
    ik.id = 1U;
    ik.name = "Arm IK";
    ik.kind = ControlRigNodeKind::TwoBoneIk;
    ik.bone = 1U;
    ik.middleBone = 2U;
    ik.endBone = 3U;
    ik.targetControl = 1U;
    ik.poleControl = 2U;
    ikRig.nodes = {ik};
    require(evaluate_control_rig(skeleton, ikRig, make_bind_pose(skeleton), values, output, nullptr, &error), error);
    auto model = compute_model_pose(skeleton, output, &error);
    require(model.size() == 4U && close(model[3].position.x, 2.0F) && close(model[3].position.y, 1.0F),
            "control-rig two-bone IK did not reach its target");

    ControlRigAsset fabrikRig;
    fabrikRig.name = "Control FABRIK";
    fabrikRig.controls = {model_control(1U, "Chain Target", {1.5F, 1.5F, 0.0F})};
    ControlRigNode fabrik;
    fabrik.id = 1U;
    fabrik.name = "Three Segment FABRIK";
    fabrik.kind = ControlRigNodeKind::Fabrik;
    fabrik.chain = {0U, 1U, 2U, 3U};
    fabrik.targetControl = 1U;
    fabrik.maximumIterations = 32U;
    fabrik.toleranceMeters = 0.0001F;
    fabrikRig.nodes = {fabrik};
    require(evaluate_control_rig(skeleton, fabrikRig, make_bind_pose(skeleton), values, output, nullptr, &error), error);
    model = compute_model_pose(skeleton, output, &error);
    require(model.size() == 4U && close(model[3].position.x, 1.5F, 2.0e-2F) &&
            close(model[3].position.y, 1.5F, 2.0e-2F), "FABRIK node did not converge on its target");
}

void test_game_world_post_animation_control_rig() {
    const SkeletonAsset skeleton = make_chain();
    ControlRigAsset rig;
    rig.name = "Gameplay Hand Rig";
    rig.controls = {
        model_control(1U, "Hand Target", {2.0F, 1.0F, 0.0F}),
        model_control(2U, "Elbow Pole", {0.0F, 0.0F, 1.0F}),
    };
    ControlRigNode ik;
    ik.id = 1U;
    ik.name = "Gameplay Arm IK";
    ik.kind = ControlRigNodeKind::TwoBoneIk;
    ik.bone = 1U;
    ik.middleBone = 2U;
    ik.endBone = 3U;
    ik.targetControl = 1U;
    ik.poleControl = 2U;
    rig.nodes = {ik};

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Rigged Character";
    std::string error;
    const GameObjectId actor = world.create_object(std::move(desc), &error);
    require(actor != kInvalidGameObjectId, error);
    require(world.animation().bind_skeleton(actor, skeleton, &error), error);
    require(world.control_rigs().bind(actor, rig, &error), error);
    require(world.control_rigs().set_control_local(
        actor, "Hand Target", make_rigid_transform({1.0F, 2.0F, 0.0F}, {}), &error), error);
    world.tick(1.0F / 60.0F);
    const LocalPose* pose = world.animation().local_pose(actor);
    require(pose != nullptr, "GameWorld lost the rigged pose");
    const auto model = compute_model_pose(skeleton, *pose, &error);
    require(model.size() == 4U && close(model[3].position.x, 1.0F) && close(model[3].position.y, 2.0F),
            "GameWorld did not evaluate the control rig after animation");
    require(world.control_rigs().control_model(actor, 1U) != nullptr,
            "control model transform was not published");
    require(world.destroy_object(actor), "rigged actor destruction failed");
    require(!world.control_rigs().has_instance(actor), "destroyed actor retained its control rig");
}

} // namespace

int main() {
    try {
        test_validation_spaces_limits_and_phases();
        test_set_copy_and_aim_nodes();
        test_ik_and_fabrik_nodes();
        test_game_world_post_animation_control_rig();
        std::cout << "DVE v1.82 control rig tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.82 control rig tests failed: " << exception.what() << '\n';
        return 1;
    }
}
