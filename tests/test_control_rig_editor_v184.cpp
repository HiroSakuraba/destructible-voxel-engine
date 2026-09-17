#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/editor_control_rig.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#if defined(DVE_V184_NATIVE_INTEGRATION_TEST)
class CountingCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view, EditorColor) const override { ++texts; }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 7;
    }
    mutable int fills{};
    mutable int outlines{};
    mutable int lines{};
    mutable int texts{};
};
#endif

void test_graph_frame_navigation_and_editing() {
    EditorControlRigPanel panel;
    panel.resize(1280, 800);
    panel.open_demo();
    auto frame = panel.frame();
    require(frame.cards.size() == 3U, "demo rig did not expose its graph cards");
    require(frame.links.size() == 2U, "demo rig did not expose typed graph links");
    require(!frame.previewLines.empty(), "rig preview did not produce native line primitives");

    const auto card = frame.cards.front();
    const auto before = panel.session().document().controlLayouts.at(card.id).position;
    require(panel.pointer_down(1, card.rect.x + 30, card.rect.y + 15), "card press was not consumed");
    require(panel.pointer_move(card.rect.x + 90, card.rect.y + 55), "card drag was not consumed");
    require(panel.pointer_up(1, card.rect.x + 90, card.rect.y + 55), "card release was not consumed");
    const auto after = panel.session().document().controlLayouts.at(card.id).position;
    require(after.x > before.x + 40.0F && after.y > before.y + 20.0F, "card drag did not commit graph coordinates");
    require(panel.session().undo(), "card move did not create an undo record");
    require(std::abs(panel.session().document().controlLayouts.at(card.id).position.x - before.x) < 0.01F,
            "undo did not restore card position");

    frame = panel.frame();
    const float zoomBefore = panel.zoom();
    require(panel.pointer_wheel(1.0F, frame.canvas.x + 100, frame.canvas.y + 100), "canvas wheel was not consumed");
    require(panel.zoom() > zoomBefore, "cursor-anchored graph zoom did not change zoom");

    frame = panel.frame();
    const std::size_t controlCount = panel.session().document().rig.controls.size();
    const int contextX = frame.canvas.x + frame.canvas.width / 2;
    const int contextY = frame.canvas.y + frame.canvas.height / 2;
    require(panel.pointer_down(3, contextX, contextY), "rig context menu did not open");
    frame = panel.frame();
    require(frame.contextActions.size() == 5U, "rig context action model is incomplete");
    const UiRect addControl = frame.contextActions.front().rect;
    require(panel.pointer_down(1, addControl.x + 5, addControl.y + 5), "context action was not consumed");
    require(panel.session().document().rig.controls.size() == controlCount + 1U,
            "context action did not add a transform control");
}

void test_pin_drag_selection_and_gizmo_capture() {
    EditorControlRigPanel panel;
    panel.resize(1280, 800);
    panel.open_demo();
    auto frame = panel.frame();
    require(!frame.links.empty(), "expected a link for interaction test");
    const auto existing = frame.links.front();
    require(panel.pointer_down(1, (existing.fromX + existing.toX) / 2, existing.fromY, 1U),
            "link disconnect gesture was not consumed");
    require(panel.session().document().links.size() == 1U, "shift-click did not disconnect a link");

    frame = panel.frame();
    const ControlRigEditorPinView* output = nullptr;
    const ControlRigEditorPinView* input = nullptr;
    for (const auto& card : frame.cards) {
        for (const auto& pin : card.pins) {
            if (pin.endpoint.pin == "Position" && pin.direction == ControlRigGraphPinDirection::Output && !output)
                output = &pin;
            if (pin.endpoint.pin == "Target" && pin.direction == ControlRigGraphPinDirection::Input)
                input = &pin;
        }
    }
    require(output && input, "could not find typed pins in the native frame");
    require(panel.pointer_down(1, output->rect.x + 5, output->rect.y + 5), "pin drag did not start");
    require(panel.pointer_move(input->rect.x + 5, input->rect.y + 5), "pin drag did not update");
    require(panel.pointer_up(1, input->rect.x + 5, input->rect.y + 5), "pin drag did not commit");
    require(panel.session().document().links.size() == 2U, "compatible pin drag did not restore the link");

    frame = panel.frame();
    const auto previewLine = *std::find_if(frame.previewLines.begin(), frame.previewLines.end(),
        [](const auto& line) { return line.control != kInvalidControlRigControlId; });
    const int pickX = (previewLine.fromX + previewLine.toX) / 2;
    const int pickY = (previewLine.fromY + previewLine.toY) / 2;
    require(panel.pointer_down(1, pickX, pickY), "preview control pick was not consumed");
    frame = panel.frame();
    require(frame.previewSelection.has_value(), "preview pick did not select a control");
    const auto selected = *frame.previewSelection;
    const auto control = std::find_if(panel.session().document().rig.controls.begin(),
        panel.session().document().rig.controls.end(), [&](const auto& item) { return item.id == selected; });
    require(control != panel.session().document().rig.controls.end(), "selected preview control does not exist");
    const float beforeX = control->defaultLocal.position.x;
    require(panel.pointer_down(1, frame.gizmoOriginX + 20, frame.gizmoOriginY), "gizmo axis press was not consumed");
    require(panel.pointer_captured(), "gizmo did not capture the pointer");
    require(panel.pointer_move(frame.gizmoOriginX + 70, frame.gizmoOriginY), "gizmo drag was not consumed");
    require(panel.pointer_up(1, frame.gizmoOriginX + 70, frame.gizmoOriginY), "gizmo release was not consumed");
    const auto committed = std::find_if(panel.session().document().rig.controls.begin(),
        panel.session().document().rig.controls.end(), [&](const auto& item) { return item.id == selected; });
    require(committed->defaultLocal.position.x > beforeX + 0.45F,
            "gizmo drag did not commit the captured transform");
    require(panel.session().undo(), "gizmo commit did not create one undo record");
}

void test_native_controller_and_renderer_integration() {
#if defined(DVE_V184_NATIVE_INTEGRATION_TEST)
    NativeEditorController controller;
    controller.resize(1280, 800);
    require(controller.dispatch_action("window.toggle_control_rig"), "native action did not open the rig editor");
    require(controller.control_rig_panel().open(), "native controller did not retain rig panel state");
    CountingCanvas canvas;
    render_native_editor(canvas, controller, 1280, 800);
    require(canvas.fills > 20 && canvas.lines > 20 && canvas.texts > 20,
            "native renderer did not paint the control-rig overlay");
    controller.key_down("a", true, false, false);
    require(!controller.control_rig_panel().session().document().selectedControls.empty(),
            "native keyboard routing did not reach the rig editor");
    require(controller.dispatch_action("window.toggle_control_rig"), "native action did not close the rig editor");
    require(!controller.control_rig_panel().open(), "rig editor remained open after toggle");
#endif
}

} // namespace

int main() {
    try {
        test_graph_frame_navigation_and_editing();
        test_pin_drag_selection_and_gizmo_capture();
        test_native_controller_and_renderer_integration();
        std::cout << "dve_v184_control_rig_editor_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v184_control_rig_editor_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
