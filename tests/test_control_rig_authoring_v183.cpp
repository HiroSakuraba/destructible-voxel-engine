#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

#include "dve/control_rig_authoring.hpp"

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 1.0e-2F) {
    return std::abs(a - b) <= tolerance;
}

SkeletonAsset make_skeleton() {
    SkeletonAsset skeleton;
    skeleton.name = "Authoring Skeleton";
    skeleton.bones = {
        {"root", -1, make_rigid_transform({}, {})},
        {"upper", 0, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
        {"lower", 1, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
        {"hand", 2, make_rigid_transform({1.0F, 0.0F, 0.0F}, {})},
    };
    return skeleton;
}

ControlRigControl target_control() {
    ControlRigControl control;
    control.name = "Hand Target";
    control.kind = ControlRigControlKind::Transform;
    control.defaultLocal = make_rigid_transform({2.0F, 1.0F, 0.0F}, {});
    return control;
}

ControlRigControl pole_control() {
    ControlRigControl control;
    control.name = "Elbow Pole";
    control.kind = ControlRigControlKind::Translation;
    control.defaultLocal = make_rigid_transform({0.0F, 0.0F, 1.0F}, {});
    return control;
}

ControlRigNode disabled_set_node() {
    ControlRigNode node;
    node.name = "Optional Root Set";
    node.kind = ControlRigNodeKind::SetBoneTransform;
    node.bone = 0U;
    node.enabled = false;
    return node;
}

ControlRigNode ik_node() {
    ControlRigNode node;
    node.name = "Arm IK";
    node.kind = ControlRigNodeKind::TwoBoneIk;
    node.bone = 1U;
    node.middleBone = 2U;
    node.endBone = 3U;
    return node;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct AuthoredFixture {
    ControlRigAuthoringSession session;
    ControlRigControlId target{};
    ControlRigControlId pole{};
    ControlRigNodeId set{};
    ControlRigNodeId ik{};
};

AuthoredFixture make_authored_fixture() {
    ControlRigAuthoringDocument document;
    document.rig.name = "Native Arm Rig";
    AuthoredFixture fixture{ControlRigAuthoringSession(std::move(document))};
    std::string error;
    fixture.target = fixture.session.add_control(target_control(), {20.0F, 40.0F}, &error);
    require(fixture.target != 0U, error);
    fixture.pole = fixture.session.add_control(pole_control(), {20.0F, 220.0F}, &error);
    require(fixture.pole != 0U, error);
    fixture.set = fixture.session.add_node(disabled_set_node(), {620.0F, 60.0F}, &error);
    require(fixture.set != 0U, error);
    fixture.ik = fixture.session.add_node(ik_node(), {340.0F, 60.0F}, &error);
    require(fixture.ik != 0U, error);
    require(fixture.session.connect(
        {ControlRigGraphEntityKind::Control, fixture.target, "Transform"},
        {ControlRigGraphEntityKind::Node, fixture.set, "Target"}, &error) != 0U, error);
    require(fixture.session.connect(
        {ControlRigGraphEntityKind::Control, fixture.target, "Position"},
        {ControlRigGraphEntityKind::Node, fixture.ik, "Target"}, &error) != 0U, error);
    require(fixture.session.connect(
        {ControlRigGraphEntityKind::Control, fixture.pole, "Position"},
        {ControlRigGraphEntityKind::Node, fixture.ik, "Pole"}, &error) != 0U, error);
    require(fixture.session.connect(
        {ControlRigGraphEntityKind::Node, fixture.ik, "Out"},
        {ControlRigGraphEntityKind::Node, fixture.set, "In"}, &error) != 0U, error);
    return fixture;
}

void test_graph_links_compilation_and_history() {
    const SkeletonAsset skeleton = make_skeleton();
    AuthoredFixture fixture = make_authored_fixture();
    std::string error;

    require(fixture.session.connect(
        {ControlRigGraphEntityKind::Control, fixture.pole, "Position"},
        {ControlRigGraphEntityKind::Node, fixture.set, "Target"}, &error) == 0U,
        "typed graph accepted a Position-to-Transform link");
    require(fixture.session.connect(
        {ControlRigGraphEntityKind::Node, fixture.set, "Out"},
        {ControlRigGraphEntityKind::Node, fixture.ik, "In"}, &error) == 0U,
        "graph accepted an execute cycle");

    ControlRigAsset compiled;
    require(fixture.session.document().compile(skeleton, compiled, &error), error);
    require(compiled.nodes.size() == 2U && compiled.nodes[0].id == fixture.ik &&
            compiled.nodes[1].id == fixture.set, "execute links did not produce a stable topological order");
    const auto diagnostics = fixture.session.document().diagnostics(skeleton);
    require(std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == ControlRigDiagnosticSeverity::Error;
    }), "valid authored graph reports an error");

    const ControlRigGraphPoint original = fixture.session.document().nodeLayouts.at(fixture.ik).position;
    require(fixture.session.move_node(fixture.ik, {500.0F, 300.0F}), "node move failed");
    require(fixture.session.can_undo() && fixture.session.undo_label() == "Move Node", "undo label is wrong");
    require(fixture.session.undo(), "node move undo failed");
    require(close(fixture.session.document().nodeLayouts.at(fixture.ik).position.x, original.x),
            "undo did not restore node position");
    require(fixture.session.redo(), "node move redo failed");
    require(close(fixture.session.document().nodeLayouts.at(fixture.ik).position.x, 500.0F),
            "redo did not restore node move");
    require(!fixture.session.remove_control(fixture.target, &error),
            "authoring session removed a referenced control");

    const std::uint64_t comment = fixture.session.add_comment(
        "Arm solve", {280.0F, 20.0F}, {620.0F, 380.0F}, &error);
    require(comment != 0U, error);
    require(fixture.session.remove_comment(comment), "comment removal failed");
    require(fixture.session.undo(), "comment removal undo failed");
    require(fixture.session.document().comments.size() == 1U, "comment undo did not restore content");
}

void test_layout_persistence_viewport_and_picking() {
    const SkeletonAsset skeleton = make_skeleton();
    AuthoredFixture fixture = make_authored_fixture();
    std::string error;
    ControlRigControlVisual visual;
    visual.shape = ControlRigControlShape::Sphere;
    visual.sizeMeters = 0.2F;
    visual.color = {0.1F, 0.8F, 1.0F, 1.0F};
    require(fixture.session.set_control_visual(fixture.target, visual, &error), error);

    ControlRigAsset compiled;
    require(fixture.session.document().compile(skeleton, compiled, &error), error);
    LocalPose output;
    std::map<ControlRigControlId, RigidTransform> controlModels;
    require(evaluate_control_rig(
        skeleton, compiled, make_bind_pose(skeleton), {}, output, &controlModels, &error), error);
    const auto lines = build_control_rig_viewport_lines(fixture.session.document(), controlModels);
    require(lines.size() >= 72U, "sphere control did not generate viewport wire geometry");
    const RigidTransform targetModel = controlModels.at(fixture.target);
    const auto picked = pick_control_rig_viewport_control(
        fixture.session.document(), controlModels,
        add(targetModel.position, {0.0F, 0.0F, -5.0F}), {0.0F, 0.0F, 1.0F});
    require(picked && *picked == fixture.target, "viewport ray did not pick the visible target control");

    const auto root = std::filesystem::temp_directory_path() / "dve_control_rig_v183_authoring";
    std::error_code filesystemError;
    std::filesystem::remove_all(root, filesystemError);
    const auto pathA = root / "arm_a.dverigui";
    const auto pathB = root / "arm_b.dverigui";
    require(write_control_rig_authoring_layout(pathA, fixture.session.document(), &error), error);
    ControlRigAuthoringDocument loaded;
    require(read_control_rig_authoring_layout(pathA, fixture.session.document().rig, loaded, &error), error);
    require(loaded.links.size() == fixture.session.document().links.size() &&
            loaded.controlVisuals.at(fixture.target).shape == ControlRigControlShape::Sphere,
            "authoring layout lost links or viewport shapes");
    require(write_control_rig_authoring_layout(pathB, loaded, &error), error);
    require(read_bytes(pathA) == read_bytes(pathB), "authoring layout serialization is not deterministic");
    {
        std::ofstream stream(pathB, std::ios::binary | std::ios::app);
        stream << "junk";
    }
    require(!read_control_rig_authoring_layout(pathB, loaded.rig, loaded, &error),
            "authoring layout reader accepted trailing data");
    std::filesystem::remove_all(root, filesystemError);
}

void test_space_switch_matching_trace_and_bake() {
    const SkeletonAsset skeleton = make_skeleton();
    const LocalPose bind = make_bind_pose(skeleton);
    AuthoredFixture fixture = make_authored_fixture();
    std::string error;
    std::map<ControlRigControlId, RigidTransform> locals;

    ControlRigAsset beforeRig;
    require(fixture.session.document().compile(skeleton, beforeRig, &error), error);
    LocalPose pose;
    std::map<ControlRigControlId, RigidTransform> beforeModels;
    require(evaluate_control_rig(skeleton, beforeRig, bind, locals, pose, &beforeModels, &error), error);
    const Float3 beforePosition = beforeModels.at(fixture.target).position;
    require(fixture.session.switch_control_space_preserve_model(
        skeleton, bind, locals, fixture.target, ControlRigSpace::Bone, 0U,
        kInvalidControlRigControlId, &error), error);
    ControlRigAsset switched;
    require(fixture.session.document().compile(skeleton, switched, &error), error);
    std::map<ControlRigControlId, RigidTransform> afterModels;
    require(evaluate_control_rig(skeleton, switched, bind, locals, pose, &afterModels, &error), error);
    require(close(afterModels.at(fixture.target).position.x, beforePosition.x) &&
            close(afterModels.at(fixture.target).position.y, beforePosition.y),
            "preserve-world space switch moved the control");

    require(fixture.session.match_control_to_bone(
        skeleton, bind, locals, fixture.target, 3U, &error), error);
    require(fixture.session.match_two_bone_ik(
        skeleton, bind, locals, fixture.ik, &error), error);
    ControlRigAsset matched;
    require(fixture.session.document().compile(skeleton, matched, &error), error);
    ControlRigEvaluationTrace trace;
    require(evaluate_control_rig_traced(skeleton, matched, bind, locals, pose, trace, nullptr, &error), error);
    require(trace.error.empty() && trace.nodes.size() == 1U && trace.nodes.front().node == fixture.ik &&
            trace.nodes.front().succeeded, "control rig trace did not capture the executed IK node");

    AnimationClipAsset source;
    source.name = "Reference Idle";
    source.durationSeconds = 1.0F;
    source.looping = true;
    AnimationClipAsset baked;
    require(bake_control_rig_clip(
        skeleton, source, matched, locals, 12.0F, "Baked Arm Rig", baked, &error), error);
    require(baked.tracks.size() == skeleton.bones.size() && baked.tracks.front().translations.size() == 13U &&
            baked.contentHash == animation_clip_content_hash(baked),
            "control rig bake did not produce a complete deterministic clip");
}

} // namespace

int main() {
    try {
        test_graph_links_compilation_and_history();
        test_layout_persistence_viewport_and_picking();
        test_space_switch_matching_trace_and_bake();
        std::cout << "DVE v1.83 control rig authoring tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.83 control rig authoring tests failed: " << exception.what() << '\n';
        return 1;
    }
}
