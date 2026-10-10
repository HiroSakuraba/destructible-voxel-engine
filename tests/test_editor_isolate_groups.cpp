// View-only isolation and group labelling (artist worklist ART-020 and ART-034):
//   * View > Isolate Selection shows and picks only the selection and its children, never
//     changes authored visibility flags or dirties the document, and toggles back,
//   * box selection and Frame All only consider what is shown,
//   * the hierarchy indents children under their parent group and gives the parent a child
//     count, and the inspector separates "Parent group" from "Named groups".
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

struct DrawnText { int x; int y; std::string value; };
class MeasuringCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int x, int y, std::string_view value, EditorColor) const override { texts.push_back({x, y, std::string(value)}); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] const DrawnText* find(std::string_view needle) const {
        for (const DrawnText& t : texts) if (t.value.find(needle) != std::string::npos) return &t;
        return nullptr;
    }
    mutable std::vector<DrawnText> texts;
};

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_native_editor_demo_document()));
    controller->resize(1280, 720);
    return controller;
}

std::set<EditorObjectId> drawn_objects(const NativeEditorController& controller) {
    std::set<EditorObjectId> ids;
    for (const EditorVoxelDrawItem& item : controller.draw_items()) ids.insert(item.objectId);
    return ids;
}

void box_select_everything(NativeEditorController& controller) {
    const UiRect v = controller.layout().viewport;
    controller.set_active_tool(EditorToolId::Select);
    controller.pointer_down(PointerButton::Primary, v.x + 2, v.y + 2);
    controller.pointer_move(v.x + v.width - 3, v.y + v.height - 3);
    controller.pointer_up(PointerButton::Primary, v.x + v.width - 3, v.y + v.height - 3);
}

void test_isolation() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    (void)controller.dispatch_action("view.frame_all");
    const std::set<EditorObjectId> all = drawn_objects(controller);
    check(all.size() >= 3, "demo draws several objects");
    controller.workspace().clear_selection();
    check(!controller.toggle_isolation(), "isolating nothing is refused");

    const EditorObjectId kept = 1004;  // Wood Crate
    controller.workspace().select_object(kept);
    const bool dirtyBefore = controller.workspace().document().dirty();
    check(controller.dispatch_action("view.isolate_selection"), "isolate the crate");
    check(drawn_objects(controller) == std::set<EditorObjectId>{kept}, "only the isolated object is drawn");
    for (const auto& [id, object] : controller.workspace().document().objects())
        check(object.flags.visible, "authored visibility untouched for object " + std::to_string(id));
    check(controller.workspace().document().dirty() == dirtyBefore, "isolation does not dirty the document");
    const MenuAction* item = std::as_const(controller.workspace().menus()).find("view.isolate_selection");
    check(item && item->checkable && item->checked, "menu shows isolation on");

    box_select_everything(controller);
    check(controller.workspace().selection_count() == 1 && controller.workspace().is_selected(kept),
          "box select only takes the isolated object");

    MeasuringCanvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.find("ISOLATED: 1 object(s)") != nullptr, "viewport says the view is isolated");

    check(controller.dispatch_action("view.isolate_selection"), "toggle isolation off");
    check(drawn_objects(controller) == all, "everything is drawn again");
    check(controller.isolated_objects().empty(), "isolation cleared");
}

void test_isolating_a_group_keeps_children_and_labels() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().clear_selection();
    controller.workspace().add_to_selection(1003);
    controller.workspace().add_to_selection(1004);
    check(controller.dispatch_action("edit.group"), "group beam and crate");
    const EditorObject* crate = controller.workspace().document().find_object(1004);
    check(crate && crate->parent.has_value(), "crate has a parent group");
    if (!crate || !crate->parent) return;
    const EditorObjectId group = *crate->parent;
    controller.workspace().select_object(group);
    check(controller.toggle_isolation(), "isolate the group");
    const auto& isolated = controller.isolated_objects();
    check(isolated.contains(group) && isolated.contains(1003) && isolated.contains(1004) && !isolated.contains(1001),
          "isolating a group keeps its children");
    (void)controller.toggle_isolation();

    controller.workspace().select_object(1004);
    MeasuringCanvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    const EditorObject* groupObject = controller.workspace().document().find_object(group);
    const DrawnText* parentLine = canvas.find("Parent group  ");
    check(parentLine && groupObject && parentLine->value == "Parent group  " + groupObject->name,
          "inspector names the parent group");
    check(canvas.find("Named groups ") != nullptr, "membership groups are labelled 'Named groups'");
    check(canvas.find("  Groups ") == nullptr, "the ambiguous 'Groups' label is gone");
    const DrawnText* groupRow = groupObject ? canvas.find(groupObject->name + "  (2)") : nullptr;
    const DrawnText* crateRow = canvas.find("Wood Crate");
    check(groupRow != nullptr, "group row shows its child count");
    check(groupRow && crateRow && crateRow->x > groupRow->x, "child rows are indented under the group");
}

} // namespace

int main() {
    test_isolation();
    test_isolating_a_group_keeps_children_and_labels();
    if (g_failures != 0) {
        std::printf("dve_editor_isolate_groups_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_isolate_groups_tests: PASS\n");
    return 0;
}
