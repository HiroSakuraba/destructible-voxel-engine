// Scatter Brush tool (artist worklist ART-093):
//   core:       one dab stays inside its disk, on the ground, keeps the spacing (also from
//               copies already placed), and is deterministic for a seed;
//   controller: a separate toolbar tool; drag paints (nothing changes until release), Shift-drag
//               erases scatter copies, Ctrl+wheel and Ctrl-drag set the radius, each stroke is one
//               undo step, strokes of one tool session share one "Scatter (N)" group, Esc cancels.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_scatter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(float a, float b, float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }

// 10 m x 10 m ground (top at y = 0.25) and a small "Rock" off to the side.
EditorDocument make_scene() {
    EditorDocument document("Scatter brush test");
    EditorObject ground(1, "Ground");
    ground.voxelSizeMeters = 0.25F;
    for (int x = 0; x < 40; ++x)
        for (int z = 0; z < 40; ++z) ground.voxels->set_voxel({x, 0, z}, 1);
    document.add_object(std::move(ground));
    EditorObject rock(2, "Rock");
    rock.voxelSizeMeters = 0.25F;
    rock.transform.position = {14.0F, 0.0F, 14.0F};
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) rock.voxels->set_voxel({x, y, z}, 2);
    document.add_object(std::move(rock));
    return document;
}

float ground_distance(Float3 a, Float3 b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

void test_dab() {
    const EditorDocument document = make_scene();
    const auto onGround = [](EditorObjectId id) { return id == 1; };
    ScatterDabSettings settings;
    settings.minSpacingMeters = 0.75F;
    settings.seed = 3;
    settings.sourceCount = 2;
    const Float3 centre{5.0F, 0.25F, 5.0F};
    const auto dab = plan_scatter_dab(document, centre, 2.0F, settings, {}, onGround);
    check(dab.size() >= 4, "a 2 m dab at 0.75 m spacing places several spots, got " + std::to_string(dab.size()));
    bool sourcesMixed = false;
    for (std::size_t i = 0; i < dab.size(); ++i) {
        check(ground_distance(dab[i].position, centre) <= 2.0F + 1.0e-4F, "spot inside the brush disk");
        check(near(dab[i].position.y, 0.25F), "spot on the ground top");
        check(dab[i].source < 2, "source index in range");
        sourcesMixed = sourcesMixed || dab[i].source != dab.front().source;
        for (std::size_t j = i + 1; j < dab.size(); ++j)
            check(ground_distance(dab[i].position, dab[j].position) >= 0.75F - 1.0e-4F, "spots keep the spacing");
    }
    check(sourcesMixed, "both sources are used");
    const auto again = plan_scatter_dab(document, centre, 2.0F, settings, {}, onGround);
    bool same = again.size() == dab.size();
    for (std::size_t i = 0; same && i < dab.size(); ++i)
        same = near(again[i].position.x, dab[i].position.x, 0.0F) && near(again[i].position.z, dab[i].position.z, 0.0F);
    check(same, "the same seed gives the same dab");

    // Spots already occupied push new ones away; a full disk takes nothing.
    std::vector<Float3> occupied;
    for (const ScatterSample& s : dab) occupied.push_back(s.position);
    for (const ScatterSample& s : plan_scatter_dab(document, centre, 2.0F, settings, occupied, onGround))
        for (const Float3& o : occupied)
            check(ground_distance(s.position, o) >= 0.75F - 1.0e-4F, "a second dab keeps clear of earlier copies");

    // Off the ground (or onto rejected objects) places nothing.
    check(plan_scatter_dab(document, {30.0F, 0.25F, 30.0F}, 2.0F, settings, {}, onGround).empty(),
          "a dab beyond the ground places nothing");
    check(plan_scatter_dab(document, centre, 2.0F, settings, {}, [](EditorObjectId) { return false; }).empty(),
          "nothing is accepted as ground: no spots");
    settings.sourceCount = 0;
    check(plan_scatter_dab(document, centre, 2.0F, settings, {}, onGround).empty(), "no sources: no spots");
}

std::size_t scatter_groups(const EditorDocument& document, std::vector<EditorObjectId>* ids = nullptr) {
    std::size_t count = 0;
    for (const auto& [id, object] : document.objects())
        if (is_scatter_group(object)) { ++count; if (ids) ids->push_back(id); }
    return count;
}

struct Screen { int x; int y; };
Screen screen_of(const NativeEditorController& controller, Float3 world) {
    const ScreenPoint p = project_world_to_screen(controller.camera(), controller.layout().viewport, world);
    return {static_cast<int>(p.x), static_cast<int>(p.y)};
}

void stroke(NativeEditorController& controller, Float3 from, Float3 to, std::uint32_t modifiers = 0, bool release = true) {
    const Screen a = screen_of(controller, from);
    controller.pointer_move(a.x, a.y, modifiers);
    controller.pointer_down(PointerButton::Primary, a.x, a.y, modifiers);
    for (int step = 1; step <= 8; ++step) {
        const float t = static_cast<float>(step) / 8.0F;
        const Screen p = screen_of(controller, {from.x + (to.x - from.x) * t, from.y, from.z + (to.z - from.z) * t});
        controller.pointer_move(p.x, p.y, modifiers);
    }
    if (release) {
        const Screen b = screen_of(controller, to);
        controller.pointer_up(PointerButton::Primary, b.x, b.y, modifiers);
    }
}

void test_toolbar_entry() {
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    NativeEditorController& controller = *owner;
    controller.resize(1280, 720);
    const auto& buttons = controller.layout().toolbarButtons;
    check(buttons.size() == kEditorToolCount && kToolbarTools.back() == EditorToolId::ScatterBrush,
          "Scatter brush has its own toolbar button");
    controller.workspace().clear_selection();
    const UiRect button = buttons.back();
    controller.pointer_down(PointerButton::Primary, button.x + 4, button.y + 4);
    controller.pointer_up(PointerButton::Primary, button.x + 4, button.y + 4);
    check(controller.active_tool() == EditorToolId::ScatterBrush, "clicking the button picks the brush");
    const auto gestures = controller.viewport_tool_gestures();
    check(!gestures.empty() && gestures.front().find("Select objects") != std::string::npos,
          "with nothing selected the hint says what to select");
    check(controller.scatter_brush_view().sourceCount == 0, "no sources");
}

void test_brush_workflow() {
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    NativeEditorController& controller = *owner;
    controller.resize(1280, 720);
    controller.workspace().select_object(1);
    (void)controller.dispatch_action("view.frame_all");
    controller.workspace().select_object(2);  // paint the rock
    controller.set_active_tool(EditorToolId::ScatterBrush);
    check(controller.scatter_brush_view().sourceCount == 1, "the selected rock is the source");
    const auto gestures = controller.viewport_tool_gestures();
    check(gestures.size() == 3 && gestures[0] == "Drag paint" && gestures[1] == "Shift-drag erase" &&
              gestures[2].find("Ctrl+wheel radius") != std::string::npos,
          "brush hint: paint, erase, radius");

    // Hover shows the brush on the ground; Ctrl+wheel and Ctrl-drag change the radius.
    const Screen centre = screen_of(controller, {5.0F, 0.25F, 5.0F});
    controller.pointer_move(centre.x, centre.y);
    auto view = controller.scatter_brush_view();
    check(view.cursor && near(view.cursor->y, 0.25F, 0.01F), "brush cursor sits on the ground");
    const float radius = view.radius;
    controller.pointer_wheel(1.0F, centre.x, centre.y, 2U);
    check(controller.scatter_brush_view().radius > radius, "Ctrl+wheel up grows the radius");
    controller.pointer_wheel(-1.0F, centre.x, centre.y, 2U);
    check(near(controller.scatter_brush_view().radius, radius, 1.0e-3F), "Ctrl+wheel down shrinks it back");
    const std::size_t undoBefore = controller.workspace().commands().size();
    controller.pointer_down(PointerButton::Primary, centre.x, centre.y, 2U);
    controller.pointer_move(centre.x + 40, centre.y, 2U);
    controller.pointer_up(PointerButton::Primary, centre.x + 40, centre.y, 2U);
    check(controller.scatter_brush_view().radius > radius, "Ctrl-drag right grows the radius");
    check(controller.workspace().commands().size() == undoBefore, "resizing adds no undo step");
    controller.set_scatter_brush_radius(1.5F);

    // Stroke 1: preview while dragging, commit on release, one undo step, one group.
    EditorDocument& document = controller.workspace().document();
    const std::size_t before = document.objects().size();
    stroke(controller, {2.0F, 0.25F, 3.0F}, {8.0F, 0.25F, 3.0F}, 0U, false);
    view = controller.scatter_brush_view();
    check(view.stroking && !view.pending.empty(), "dragging shows pending spots");
    check(document.objects().size() == before, "nothing is added until release");
    const Screen end = screen_of(controller, {8.0F, 0.25F, 3.0F});
    controller.pointer_up(PointerButton::Primary, end.x, end.y);
    const std::size_t firstCopies = document.objects().size() - before - 1;
    std::vector<EditorObjectId> groups;
    check(scatter_groups(document, &groups) == 1 && firstCopies > 3, "stroke adds one group and several copies, got " +
          std::to_string(firstCopies));
    const EditorObjectId group = groups.empty() ? 0 : groups.front();
    check(document.find_object(group) && document.find_object(group)->name == "Scatter (" + std::to_string(firstCopies) + ")",
          "group is named with its copy count");
    check(controller.workspace().commands().size() == undoBefore + 1, "a stroke is one undo step");
    for (const EditorObjectId child : document.children_of(group)) {
        const EditorObject* copy = document.find_object(child);
        check(copy && copy->name == "Rock", "copies are rocks");
        const EditorObjectBounds b = object_world_bounds(*copy);
        check(near(b.minimum.y, 0.25F, 0.01F), "copies stand on the ground");
    }

    // Stroke 2 over the same line adds few or none (spacing), a parallel one joins the same group.
    stroke(controller, {2.0F, 0.25F, 7.0F}, {8.0F, 0.25F, 7.0F});
    const std::size_t total = document.children_of(group).size();
    check(scatter_groups(document) == 1 && total > firstCopies, "second stroke joins the session group");
    check(document.find_object(group)->name == "Scatter (" + std::to_string(total) + ")", "group count updated");
    std::vector<Float3> feet;
    for (const EditorObjectId child : document.children_of(group)) {
        const EditorObjectBounds b = object_world_bounds(*document.find_object(child));
        feet.push_back({(b.minimum.x + b.maximum.x) * 0.5F, b.minimum.y, (b.minimum.z + b.maximum.z) * 0.5F});
    }
    for (std::size_t i = 0; i < feet.size(); ++i)
        for (std::size_t j = i + 1; j < feet.size(); ++j)
            check(ground_distance(feet[i], feet[j]) >= controller.scatter_settings().minSpacingMeters - 1.0e-3F,
                  "copies across strokes keep the spacing");
    check(controller.dispatch_action("edit.undo"), "undo stroke 2");
    check(document.children_of(group).size() == firstCopies &&
              document.find_object(group)->name == "Scatter (" + std::to_string(firstCopies) + ")",
          "undo removes only the last stroke and restores the name");
    check(controller.dispatch_action("edit.redo") && document.children_of(group).size() == total, "redo");

    // Shift-drag erases copies under the brush, one undo step; the original rock is never erased.
    stroke(controller, {2.0F, 0.25F, 3.0F}, {8.0F, 0.25F, 3.0F}, 1U, false);
    view = controller.scatter_brush_view();
    check(view.erasing && !view.erasing_ids.empty(), "erase stroke marks copies");
    controller.pointer_up(PointerButton::Primary, end.x, end.y, 1U);
    const std::size_t remaining = document.children_of(group).size();
    check(remaining < total && remaining > 0, "erase removed the copies along the first line only");
    check(document.find_object(2) != nullptr, "the source rock is untouched");
    check(document.find_object(group)->name == "Scatter (" + std::to_string(remaining) + ")", "name follows the erase");
    check(controller.dispatch_action("edit.undo") && document.children_of(group).size() == total, "undo restores erased copies");

    // Esc mid-stroke cancels without touching the document.
    const std::size_t count = document.objects().size();
    const std::size_t undoCount = controller.workspace().commands().size();
    stroke(controller, {2.0F, 0.25F, 9.0F}, {8.0F, 0.25F, 9.0F}, 0U, false);
    controller.key_down("escape", false, false, false);
    check(!controller.scatter_brush_view().stroking && controller.scatter_brush_view().pending.empty(), "Esc ends the stroke");
    controller.pointer_up(PointerButton::Primary, end.x, end.y);
    check(document.objects().size() == count && controller.workspace().commands().size() == undoCount,
          "a cancelled stroke changes nothing");

    // Undo back past the first stroke removes the group; the next stroke makes a fresh one.
    while (scatter_groups(document) > 0 && controller.dispatch_action("edit.undo")) {}
    check(scatter_groups(document) == 0 && document.objects().size() == before, "undo removes every stroke");
    stroke(controller, {2.0F, 0.25F, 5.0F}, {8.0F, 0.25F, 5.0F});
    check(scatter_groups(document) == 1, "painting after undo re-creates the group");

    // A new tool session starts a new group.
    controller.set_active_tool(EditorToolId::Select);
    controller.workspace().select_object(2);
    controller.set_active_tool(EditorToolId::ScatterBrush);
    stroke(controller, {2.0F, 0.25F, 1.0F}, {8.0F, 0.25F, 1.0F});
    check(scatter_groups(document) == 2, "a new tool session gets its own group");

    // The brush draws its ring and label.
    struct Counting final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override { ++lines; }
        void text(int, int, std::string_view value, EditorColor) const override {
            if (value.starts_with("Scatter 1.5 m")) label = true;
            if (value.find("Scatter (") != std::string_view::npos) {
                ++groupRows;
                if (value.find(")  (") != std::string_view::npos) doubledCount = true;
            }
        }
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        mutable int lines{};
        mutable bool label{};
        mutable int groupRows{};
        mutable bool doubledCount{};
    } canvas;
    controller.pointer_move(centre.x, centre.y);
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.label, "the brush ring is labelled with its radius");
    check(canvas.groupRows >= 2 && !canvas.doubledCount, "hierarchy shows 'Scatter (N)' without repeating the count");
}

} // namespace

int main() {
    test_dab();
    test_toolbar_entry();
    test_brush_workflow();
    if (g_failures != 0) {
        std::printf("dve_editor_scatter_brush_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_scatter_brush_tests: PASS\n");
    return 0;
}
