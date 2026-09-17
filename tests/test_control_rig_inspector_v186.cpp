#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/editor_control_rig.hpp"

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

const ControlRigInspectorPropertyView& property(
    const ControlRigEditorFrame& frame, std::string_view id) {
    const auto found = std::find_if(frame.inspectorProperties.begin(), frame.inspectorProperties.end(),
        [id](const auto& item) { return item.id == id; });
    if (found == frame.inspectorProperties.end())
        throw std::runtime_error("missing inspector property: " + std::string(id));
    return *found;
}

void test_multi_control_transaction_and_undo() {
    EditorControlRigPanel panel;
    panel.open_demo();
    auto& document = panel.session().document();
    for (const auto& control : document.rig.controls) document.selectedControls.insert(control.id);
    const std::uint64_t revision = document.revision;
    std::string error;
    require(panel.adjust_inspector_property("visible", -1, &error), error);
    require(document.revision == revision + 1U, "multi-control edit did not create exactly one revision");
    for (const auto& control : document.rig.controls)
        require(!document.controlVisuals.at(control.id).visible, "multi-control visual edit was incomplete");
    require(panel.session().undo_label() == "Edit Control Visuals", "multi-control undo label is incorrect");
    require(panel.session().undo(), "multi-control edit could not be undone");
    for (const auto& control : document.rig.controls)
        require(document.controlVisuals.at(control.id).visible, "undo did not restore every control visual");
}

void test_name_edit_and_rejected_batch_are_atomic() {
    EditorControlRigPanel panel;
    panel.open_demo();
    auto& document = panel.session().document();
    document.selectedControls.insert(document.rig.controls.front().id);
    const auto nameRow = property(panel.frame(), "name").row;
    require(panel.pointer_down(1, nameRow.x + 8, nameRow.y + 8), "name row click was not consumed");
    require(panel.text_input("Wrist Goal"), "native inspector text was not consumed");
    require(panel.key_down("enter", false, false, false), "name commit key was not consumed");
    require(document.rig.controls.front().name == "Wrist Goal", "control name was not committed");

    const std::uint64_t revision = document.revision;
    ControlRigControl duplicate = document.rig.controls.front();
    duplicate.name = document.rig.controls.back().name;
    std::string error;
    require(!panel.session().update_controls(
                panel.skeleton(), std::span<const ControlRigControl>(&duplicate, 1U), &error),
            "duplicate control name passed validation");
    require(!error.empty() && document.revision == revision &&
            document.rig.controls.front().name == "Wrist Goal",
            "rejected control edit changed document state or history");
}

void test_link_incompatible_control_kind_rolls_back() {
    EditorControlRigPanel panel;
    panel.open_demo();
    auto& document = panel.session().document();
    document.selectedControls.insert(document.rig.controls.front().id);
    const std::uint64_t revision = document.revision;
    std::string error;
    require(!panel.adjust_inspector_property("kind", -1, &error),
            "control kind change removed a linked Position pin without rejection");
    require(!error.empty() && document.revision == revision &&
            document.rig.controls.front().kind == ControlRigControlKind::Transform,
            "failed kind edit was not rolled back atomically");
}

void test_node_multi_edit_and_bone_search() {
    SkeletonAsset skeleton = make_control_rig_editor_demo_skeleton();
    ControlRigAuthoringSession authoring(make_control_rig_editor_demo_document());
    ControlRigNode setBone;
    setBone.name = "Finger Driver";
    setBone.bone = 0U;
    setBone.targetControl = authoring.document().rig.controls.front().id;
    std::string error;
    const ControlRigNodeId extra = authoring.add_node(setBone, {720.0F, 100.0F}, &error);
    require(extra != 0U, error);
    authoring.clear_history();

    EditorControlRigPanel panel;
    panel.open_document(skeleton, authoring.document());
    auto& document = panel.session().document();
    for (const auto& node : document.rig.nodes) document.selectedNodes.insert(node.id);
    const std::uint64_t revision = document.revision;
    require(panel.adjust_inspector_property("weight", -1, &error), error);
    require(document.revision == revision + 1U, "multi-node weight edit was not one transaction");
    for (const auto& node : document.rig.nodes) require(node.weight == 0.95F, "multi-node weight edit was incomplete");
    require(panel.session().undo(), "multi-node edit could not be undone");

    document.selectedNodes.clear();
    document.selectedNodes.insert(extra);
    const auto boneRow = property(panel.frame(), "bone").row;
    require(panel.pointer_down(1, boneRow.x + 8, boneRow.y + 8), "bone row click was not consumed");
    require(panel.text_input("hand"), "bone search text was not consumed");
    require(panel.key_down("enter", false, false, false), "bone search commit was not consumed");
    const auto found = std::find_if(document.rig.nodes.begin(), document.rig.nodes.end(),
        [extra](const auto& node) { return node.id == extra; });
    require(found != document.rig.nodes.end() && found->bone == 3U,
            "case-insensitive bone search did not resolve and commit the unique bone");
}

void test_frame_mixed_values_and_reference_cycling() {
    EditorControlRigPanel panel;
    panel.open_demo();
    auto& document = panel.session().document();
    for (const auto& control : document.rig.controls) document.selectedControls.insert(control.id);
    const ControlRigEditorFrame mixedFrame = panel.frame();
    const auto& kind = property(mixedFrame, "kind");
    require(kind.mixed && kind.value == "Multiple", "mixed selection was not represented explicitly");

    document.selectedControls.clear();
    ControlRigNode setBone;
    setBone.name = "Reference Cycle";
    setBone.bone = 0U;
    setBone.targetControl = document.rig.controls.front().id;
    std::string error;
    const ControlRigNodeId nodeId = panel.session().add_node(setBone, {760.0F, 300.0F}, &error);
    require(nodeId != 0U, error);
    document.selectedNodes.insert(nodeId);
    require(panel.adjust_inspector_property("bone", 1, &error), error);
    const auto found = std::find_if(document.rig.nodes.begin(), document.rig.nodes.end(),
        [nodeId](const auto& node) { return node.id == nodeId; });
    require(found != document.rig.nodes.end() && found->bone == 1U,
            "bone reference cycling did not advance");
}

} // namespace

int main() {
    try {
        test_multi_control_transaction_and_undo();
        test_name_edit_and_rejected_batch_are_atomic();
        test_link_incompatible_control_kind_rolls_back();
        test_node_multi_edit_and_bone_search();
        test_frame_mixed_values_and_reference_cycling();
        std::cout << "dve_v186_control_rig_inspector_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v186_control_rig_inspector_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
