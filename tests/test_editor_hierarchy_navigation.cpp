#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::editor;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
EditorDocument scene() {
    EditorDocument doc("Hierarchy");
    auto add = [&](EditorObjectId id, const char* name, std::optional<EditorObjectId> parent = {}) {
        EditorObject object(id, name); object.parent = parent;
        doc.add_object(std::move(object));
    };
    add(1, "Room"); add(2, "Furniture", 1); add(3, "Chair", 2); add(4, "Lamp", 1);
    add(5, "Garden"); add(6, "CHAIR outdoors", 5); add(7, "Unrelated");
    return doc;
}
std::unique_ptr<NativeEditorController> controller(EditorDocument doc = scene()) {
    auto result = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(doc)));
    result->resize(1280, 720); return result;
}
void click(NativeEditorController& c, UiRect rect, int inset = 2) {
    require(rect.width > inset, "click target is visible");
    c.pointer_down(PointerButton::Primary, rect.x + inset, rect.y + rect.height / 2);
    c.pointer_up(PointerButton::Primary, rect.x + inset, rect.y + rect.height / 2);
}
void key(NativeEditorController& c, const char* text) { c.key_down(text, false, false, false); }
void filter(NativeEditorController& c, const char* text) {
    click(c, c.layout().hierarchyFilterBox); c.text_input(text); key(c, "enter");
}
UiRect row(NativeEditorController& c, EditorObjectId id) {
    const auto order = c.hierarchy_order(); const auto it = std::find(order.begin(), order.end(), id);
    require(it != order.end(), "row is in hierarchy order");
    return c.layout().hierarchyRows[static_cast<std::size_t>(it - order.begin())];
}
void test_fold_and_filter() {
    auto owner = controller(); auto& c = *owner;
    c.workspace().select_object(3);
    const bool dirtyBefore = c.workspace().document().dirty();
    require(!c.workspace().commands().can_undo(), "initial undo stack empty");
    click(c, c.layout().hierarchyDisclosureButtons[1]);
    require(c.hierarchy_order() == std::vector<EditorObjectId>({1,2,4,5,6,7}), "nested fold hides its child");
    click(c, c.layout().hierarchyDisclosureButtons[1]);
    require(!c.hierarchy_collapsed(2), "a second disclosure click expands without firing a viewport shortcut");
    click(c, c.layout().hierarchyDisclosureButtons[1]);
    require(c.workspace().selected_object() == 3, "fold preserves selection");
    require(c.hierarchy_drag().sourceId == 0, "disclosure never arms reparent drag");
    require(c.workspace().document().find_object(3)->parent == 2, "fold never reparents");
    require(c.workspace().document().dirty() == dirtyBefore && !c.workspace().commands().can_undo(), "view operations do not dirty or add undo");
    require(c.set_hierarchy_expanded(1, false), "fold root");
    require(c.hierarchy_order() == std::vector<EditorObjectId>({1,5,6,7}), "root fold hides entire subtree");
    filter(c, "chair");
    require(c.hierarchy_order() == std::vector<EditorObjectId>({1,2,3,5,6}), "filter includes both matching paths, case insensitive");
    require(!c.set_hierarchy_expanded(1, false), "filtered paths cannot be hidden");
    require(c.status().text.find("Clear") != std::string::npos, "filter fold refusal gives a route");
    require(c.hierarchy_collapsed(1) && c.hierarchy_collapsed(2), "filter preserves folded view state");
    key(c, "left"); require(c.workspace().selected_object() == 2, "filtered left moves to contextual parent");
    click(c, c.layout().hierarchyFilterBox); key(c, "escape");
    require(c.hierarchy_order() == std::vector<EditorObjectId>({1,5,6,7}), "clearing filter restores folded view");
    require(c.dispatch_action("hierarchy.expand_all"), "expand all command");
    require(c.hierarchy_order().size() == 7, "all children expanded");
    require(c.dispatch_action("hierarchy.collapse_all"), "collapse all command");
    require(c.hierarchy_order() == std::vector<EditorObjectId>({1,5,7}), "collapse all shows roots only");
    struct Canvas final : IEditorCanvas {
        mutable std::vector<std::string> labels;
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override {}
        void text(int, int, std::string_view value, EditorColor) const override { labels.emplace_back(value); }
        int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    } canvas;
    render_native_editor(canvas, c, 1280, 720);
    require(std::count(canvas.labels.begin(), canvas.labels.end(), ">") == 2, "folded parents have visible disclosure indicators");
    require(!c.set_hierarchy_expanded(7, false) && !c.set_hierarchy_expanded(9999, true), "leaf/missing id safe");
}
void test_keyboard_and_reveal() {
    auto owner = controller(); auto& c = *owner;
    click(c, row(c, 1), 50);
    key(c, "right"); require(c.workspace().selected_object() == 2, "right enters first child");
    key(c, "right"); require(c.workspace().selected_object() == 3, "right enters grandchild");
    key(c, "left"); require(c.workspace().selected_object() == 2, "left goes to parent");
    key(c, "left"); require(c.hierarchy_collapsed(2), "left folds expanded parent");
    key(c, "down"); require(c.workspace().selected_object() == 4, "down skips folded child");
    key(c, "up"); require(c.workspace().selected_object() == 2, "up follows visible order");
    key(c, "right"); require(!c.hierarchy_collapsed(2) && c.workspace().selected_object() == 2, "right unfolds before entering");
    key(c, "end"); require(c.workspace().selected_object() == 7, "end selects last displayed row");
    key(c, "down"); require(c.workspace().selected_object() == 7, "down does not wrap");
    key(c, "home"); require(c.workspace().selected_object() == 1, "home selects first row");
    (void)c.dispatch_action("hierarchy.collapse_all");
    c.workspace().select_object(3); c.workspace().add_to_selection(6); c.workspace().add_to_selection(3);
    const auto selected = c.workspace().selected_objects();
    filter(c, "unrelated");
    c.workspace().set_panel_visible(PanelId::SceneHierarchy, false); c.resize(1280, 720);
    require(c.dispatch_action("hierarchy.reveal_selection"), "reveal command");
    require(c.workspace().selected_objects() == selected && c.workspace().selected_object() == 3, "reveal preserves multiselection and primary");
    require(c.hierarchy_filter().empty() && !c.hierarchy_collapsed(1) && !c.hierarchy_collapsed(2), "reveal clears filter and opens ancestors");
    require(c.hierarchy_collapsed(5), "unrelated branch stays folded");
    require(row(c, 3).width > 0 && c.focus_region() == EditorFocusRegion::SceneHierarchy, "reveal opens panel and focuses hierarchy");
    c.workspace().clear_selection();
    require(!c.dispatch_action("hierarchy.reveal_selection"), "reveal without selection refused");
}
void test_scroll_and_new_scene() {
    EditorDocument doc("Large");
    doc.add_object(EditorObject(1, "Root"));
    for (EditorObjectId id = 2; id <= 100; ++id) {
        EditorObject object(id, (id == 100 ? "Needle" : "Item") + std::to_string(id)); object.parent = 1;
        doc.add_object(std::move(object));
    }
    auto owner = controller(std::move(doc)); auto& c = *owner;
    c.workspace().select_object(100);
    require(row(c, 100).width == 0, "offscreen row has no hit target");
    require(c.reveal_selection_in_hierarchy(), "reveal offscreen selection");
    require(row(c, 100).width > 0 && c.layout().hierarchyScroll > 0, "reveal scrolls to selected row");
    key(c, "home"); require(row(c, 1).width > 0 && c.layout().hierarchyScroll == 0, "keyboard scrolls back to top");
    key(c, "end"); require(row(c, 100).width > 0, "end scrolls to bottom");
    filter(c, "needle");
    require(c.layout().hierarchyScroll == 0 && c.hierarchy_order() == std::vector<EditorObjectId>({1,100}), "new filter starts at first matching path");
    click(c, c.layout().hierarchyFilterBox); key(c, "escape");
    (void)c.dispatch_action("hierarchy.collapse_all");
    filter(c, "needle");
    require(c.dispatch_action("file.new_scene"), "new scene requested");
    if (!c.pending_confirmation_message().empty()) key(c, "enter");
    require(c.hierarchy_filter().empty() && !c.hierarchy_collapsed(1) && c.layout().hierarchyScroll == 0, "new scene resets view ids and filter");
    const auto path = std::filesystem::temp_directory_path() / "dve-hierarchy-navigation-open.dvescene";
    auto saved = scene(); require(saved.save_transactional(path).success, "save fixture");
    require(c.open_scene_file(path), "open scene fixture");
    if (c.pending_destructive_confirmation()) c.confirm_pending_destructive_action();
    (void)c.dispatch_action("hierarchy.collapse_all"); filter(c, "chair");
    require(c.open_scene_file(path), "reopen same scene");
    if (c.pending_destructive_confirmation()) c.confirm_pending_destructive_action();
    require(c.hierarchy_order().size() == 7 && c.hierarchy_filter().empty(), "opening a scene resets folds and filter despite reused ids");
    std::error_code ignored; std::filesystem::remove(path, ignored);
}
}
int main() {
    try { test_fold_and_filter(); test_keyboard_and_reveal(); test_scroll_and_new_scene(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Hierarchy navigation passed\n";
}
