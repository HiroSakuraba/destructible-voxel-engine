// Regressions from the native editor GUI exploration: the command palette (typing, unavailable
// commands, empty searches), the Create Exact Tube prompt keeping itself open on bad input, the
// Hierarchy listing a freshly created polygon object at once, wheel dolly after a Hierarchy click,
// the Move gizmo following the object during a drag, Inspector mass and collision of exact-collision
// polygons, pointer modifier bits and the Console collapsing repeated lines.
#include "dve/collision_convex_set.hpp"
#include "dve/editor_diagnostics.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_workspace.hpp"
#include "dve/voxel_metric.hpp"

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

std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Quirks");
    EditorObject cube(1, "Cube");
    cube.polygon = make_editor_polygon_cube(1);
    document.add_object(std::move(cube));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600, 900);
    c->workspace().select_object(1);
    c->camera().position = {.5F, 1.5F, 6.0F};
    c->camera().target = {.5F, .5F, .5F};
    return c;
}

void type_text(NativeEditorController& c, std::string_view text, bool hostSuppliesText) {
    for (const char ch : text) {
        const std::string key(1U, ch);
        c.key_down(key, false, false, false);
        if (hostSuppliesText) c.text_input(key);
    }
}

void modifier_bits() {
    check(pointer_modifier_mask(false, false, false) == 0U, "no modifiers");
    check(pointer_modifier_mask(true, false, false) == 1U, "shift is bit 1");
    check(pointer_modifier_mask(false, true, false) == 2U, "ctrl is bit 2");
    check(pointer_modifier_mask(false, false, true) == 4U, "alt is bit 4");
    check(pointer_modifier_mask(true, true, true) == 7U, "all three");
}

void palette_typing() {
    for (const bool host : {true, false}) {
        auto owner = make_controller();
        auto& c = *owner;
        c.set_host_supplies_text_input(host);
        check(c.dispatch_action("help.command_palette") && c.command_palette_open(), "palette opens");
        type_text(c, "exact", host);
        check(c.command_palette_query() == "exact",
              std::string("each key types once (host supplies text: ") + (host ? "yes" : "no") + "), got '" +
                  std::string(c.command_palette_query()) + "'");
    }
    auto owner = make_controller();
    auto& c = *owner;
    c.set_host_supplies_text_input(true);
    check(c.dispatch_action("help.command_palette"), "palette opens");
    c.text_input(std::string_view("caf\xC3\xA9"));
    check(c.command_palette_query() == "caf\xC3\xA9", "non-ASCII text reaches the palette query");
    c.key_down("backspace", false, false, false);
    check(c.command_palette_query() == "caf", "backspace removes a whole UTF-8 character");
}

void palette_unavailable_and_empty() {
    auto owner = make_controller();
    auto& c = *owner;
    check(c.dispatch_action("help.command_palette"), "palette opens");
    type_text(c, "zzqqxxnomatch", false);
    check(c.command_palette_results(10).empty(), "nothing matches the nonsense query");
    c.key_down("enter", false, false, false);
    check(c.command_palette_open(), "an empty search leaves the palette open");
    check(!c.command_palette_message().empty(), "an empty search says why nothing ran");
    c.key_down("backspace", false, false, false);
    check(c.command_palette_message().empty(), "editing the query clears the message");

    // An unavailable command: Enter keeps the palette open and says why; it does not run another one.
    // Mesh Exact Cope needs two selected tubes, so it is unavailable here.
    c.key_down("escape", false, false, false);
    check(!c.command_palette_open(), "escape closes the palette");
    check(c.dispatch_action("help.command_palette"), "palette opens again");
    check(c.command_palette_message().empty(), "reopening starts without a stale message");
    bool found = false;
    for (const char* query : {"cope", "miter", "exact cope", "exact miter"}) {
        while (!c.command_palette_query().empty()) c.key_down("backspace", false, false, false);
        type_text(c, query, false);
        const auto results = c.command_palette_results(64);
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (results[i].enabled) continue;
            found = true;
            for (std::size_t step = 0; step < i; ++step) c.key_down("down", false, false, false);
            break;
        }
        if (found) break;
    }
    check(found, "there is an unavailable command to try");
    if (found) {
        const std::size_t objectsBefore = c.workspace().document().objects().size();
        c.key_down("enter", false, false, false);
        check(c.command_palette_open(), "Enter on an unavailable command keeps the palette open");
        check(c.command_palette_message().find("unavailable") != std::string::npos,
              "the palette says the command is unavailable: " + std::string(c.command_palette_message()));
        check(c.workspace().document().objects().size() == objectsBefore, "nothing ran");
    }
}

void exact_tube_prompt_stays_open() {
    auto owner = make_controller();
    auto& c = *owner;
    check(c.dispatch_action("polygon.create_exact_tube"), "exact tube prompt opens");
    check(c.text_edit().kind == TextEditKind::ExactTube, "the prompt is the Exact Tube one");
    const std::size_t objects = c.workspace().document().objects().size();
    for (const char ch : std::string("round x")) c.text_input(std::string_view(&ch, 1U));
    c.key_down("enter", false, false, false);
    check(c.text_edit().kind == TextEditKind::ExactTube, "an invalid spec keeps the prompt open");
    check(!c.text_edit().error.empty(), "and says what is wrong");
    check(c.text_edit().buffer == "round x", "the typed text is kept for editing");
    // Keys typed after the error edit the prompt: they are not viewport shortcuts.
    c.text_input(std::string_view("y"));
    check(c.text_edit().error.empty(), "typing clears the stale error");
    check(c.text_edit().buffer == "round xy", "typing continues from the kept text");
    check(c.workspace().document().objects().size() == objects, "nothing was created");
    c.key_down("escape", false, false, false);
    check(c.text_edit().kind == TextEditKind::Inactive, "escape closes the prompt");
}

void hierarchy_lists_new_polygon() {
    auto owner = make_controller();
    auto& c = *owner;
    check(c.layout().hierarchyRows.size() == 1U, "one row to start with");
    check(c.dispatch_action("create.polygon"), "polygon cube is created");
    check(c.workspace().document().objects().size() == 2U, "two objects now");
    check(c.layout().hierarchyRows.size() == 2U, "the new polygon object has its Hierarchy row at once");
    // An object that appears by another route is listed on the next update, not after the next operation.
    EditorObject extra(c.workspace().document().allocate_object_id(), "Extra");
    extra.polygon = make_editor_polygon_cube(extra.id);
    c.workspace().document().add_object(std::move(extra));
    c.update(0.0F);
    check(c.layout().hierarchyRows.size() == 3U, "an object added directly is listed after update()");
}

float camera_distance(const NativeEditorController& c) {
    return length(subtract(c.camera().position, c.camera().target));
}

void wheel_after_hierarchy_click() {
    auto owner = make_controller();
    auto& c = *owner;
    c.update(0.0F);
    check(!c.layout().hierarchyRows.empty(), "hierarchy row exists");
    const UiRect row = c.layout().hierarchyRows.front();
    c.pointer_down(PointerButton::Primary, row.x + row.width / 2, row.y + row.height / 2);
    c.pointer_up(PointerButton::Primary, row.x + row.width / 2, row.y + row.height / 2);
    const float before = camera_distance(c);
    const UiRect view = c.layout().viewport;
    c.pointer_wheel(1.0F, view.x + view.width / 2, view.y + view.height / 2, 0U);
    check(camera_distance(c) < before - 1e-4F, "the wheel dollies the view right after a Hierarchy click");
}

void gizmo_follows_the_drag() {
    EditorDocument document("Gizmo");
    EditorObject mover(1, "Mover");
    mover.voxelSizeMeters = 0.5F;
    (void)mover.voxels->set_voxel({0, 0, 0}, 1);
    document.add_object(std::move(mover));
    NativeEditorController c(EditorWorkspace(std::move(document)));
    c.resize(1600, 900);
    c.camera().position = {0.0F, 2.0F, 8.0F};
    c.camera().target = {0.0F, 0.0F, 0.0F};
    c.workspace().select_object(1);
    c.set_active_tool(EditorToolId::Translate);
    GizmoScreenAxis xAxis{};
    for (const GizmoScreenAxis& axis : c.gizmo_axes()) if (axis.axis == 1) xAxis = axis;
    check(xAxis.axis == 1, "x handle exists");
    const int gx = static_cast<int>(xAxis.start.x + (xAxis.end.x - xAxis.start.x) * 0.7F);
    const int gy = static_cast<int>(xAxis.start.y + (xAxis.end.y - xAxis.start.y) * 0.7F);
    c.pointer_move(gx, gy);
    c.pointer_down(PointerButton::Primary, gx, gy);
    c.pointer_move(gx + 80, gy);
    const float moved = c.workspace().document().find_object(1)->transform.position.x;
    check(moved > 0.05F, "the object moved with the drag");
    float drawnStart = 0.0F, dragMathStart = 0.0F;
    for (const GizmoScreenAxis& axis : c.gizmo_axes(true)) if (axis.axis == 1) drawnStart = axis.start.x;
    for (const GizmoScreenAxis& axis : c.gizmo_axes(false)) if (axis.axis == 1) dragMathStart = axis.start.x;
    check(drawnStart > dragMathStart + 1.0F, "the drawn gizmo moved with the object while the drag maths keeps its start pivot");
    c.pointer_up(PointerButton::Primary, gx + 80, gy);
}

void diagnostics_of_exact_collision() {
    EditorDocument document("Diag");
    EditorObject tube(1, "Tube");
    tube.polygon = make_editor_polygon_cube(1);
    ConvexPrism piece;
    piece.bottom = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}};
    piece.top = {{0, 2, 0}, {1, 2, 0}, {1, 2, 1}, {0, 2, 1}};  // 1 x 2 x 1 m box = 2 m^3
    const std::vector<ConvexPrism> pieces{piece, piece};
    Component component;
    component.id = 1;
    component.type = std::string(kConvexSetComponent);
    component.properties["pieces"] = encode_convex_set(pieces);
    tube.components.push_back(component);
    document.add_object(std::move(tube));
    EditorObject plain(2, "Plain");
    plain.polygon = make_editor_polygon_cube(2);
    document.add_object(std::move(plain));
    const EditorMaterialLibrary materials;
    const auto exact = analyze_editor_object(*document.find_object(1), materials);
    check(exact.collisionConvexPieces == 2U && exact.massKnown, "exact pieces are counted and the mass is known");
    check(std::abs(exact.massKilograms - 4000.0) < 1.0, "mass is the exact volume x 1000 kg/m^3, got " + std::to_string(exact.massKilograms));
    const auto plainDiagnostics = analyze_editor_object(*document.find_object(2), materials);
    check(!plainDiagnostics.massKnown && plainDiagnostics.massKilograms == 0.0, "a polygon without a volume model has an unknown mass");
    const auto selection = analyze_editor_selection(document, materials, {1, 2});
    check(selection.collisionConvexPieces == 2U && selection.objectsWithoutMass == 1U && selection.massKilograms > 3999.0,
          "the selection sums the known mass and counts the objects without one");
    document.find_object(1)->flags.collisionEnabled = false;
    check(analyze_editor_object(*document.find_object(1), materials).collisionConvexPieces == 0U, "collision off: no pieces");
}

void console_collapses_repeats() {
    EditorLogStore log;
    for (int i = 0; i < 5; ++i) log.add(EditorLogLevel::Info, "1 polygon triangle faces selected");
    log.add(EditorLogLevel::Info, "Moved");
    log.add(EditorLogLevel::Info, "1 polygon triangle faces selected");
    check(log.entries().size() == 3U, "five repeats collapse into one entry, the later one is separate");
    check(log.entries().front().repeats == 5U, "the repeat count is kept");
    log.add(EditorLogLevel::Warning, "1 polygon triangle faces selected");
    check(log.entries().size() == 4U, "a different level is a different entry");
}
}  // namespace

int main() {
    modifier_bits();
    palette_typing();
    palette_unavailable_and_empty();
    exact_tube_prompt_stays_open();
    hierarchy_lists_new_polygon();
    wheel_after_hierarchy_click();
    gizmo_follows_the_drag();
    diagnostics_of_exact_collision();
    console_collapses_repeats();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("editor GUI quirk tests passed\n");
    return 0;
}
