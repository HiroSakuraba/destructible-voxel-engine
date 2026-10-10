// Scatter follow-ups (artist worklist ART-093):
//   core:       per-copy random turn and scale (voxel size) without moving spots, brush density,
//               several surfaces in one fill, settings stored on the scatter group (component),
//               saved and loaded with the scene, dropped from runtime exports;
//   controller: the Inspector shows scatter settings while filling or brushing (type, toggle,
//               wheel), Set Scatter Sources then several surfaces, Scatter Brush action and
//               shortcut, settings come back from a selected or the newest scatter group.
#include "dve/editor_accessibility.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_scatter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
bool near(float a, float b, float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }

EditorObject make_slab(EditorObjectId id, std::string name, Float3 position) {
    EditorObject slab(id, std::move(name));
    slab.voxelSizeMeters = 0.25F;
    slab.transform.position = position;
    for (int x = 0; x < 24; ++x)
        for (int z = 0; z < 24; ++z) slab.voxels->set_voxel({x, 0, z}, 1);
    return slab;
}

// Two 6 m slabs ("Ground A" at x 0..6, "Ground B" at x 20..26), a 2x2x2 "Rock" off to the
// side, and a Text object (not a voxel surface).
EditorDocument make_scene() {
    EditorDocument document("Scatter settings test");
    document.add_object(make_slab(1, "Ground A", {0.0F, 0.0F, 0.0F}));
    document.add_object(make_slab(2, "Ground B", {20.0F, 0.0F, 0.0F}));
    EditorObject rock(3, "Rock");
    rock.voxelSizeMeters = 0.25F;
    rock.transform.position = {10.0F, 0.0F, 12.0F};
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) rock.voxels->set_voxel({x, y, z}, 2);
    document.add_object(std::move(rock));
    return document;
}

std::vector<EditorObjectId> scatter_groups(const EditorDocument& document) {
    std::vector<EditorObjectId> ids;
    for (const auto& [id, object] : document.objects())
        if (is_scatter_group(object)) ids.push_back(id);
    return ids;
}

void test_variation() {
    const EditorDocument document = make_scene();
    ScatterSettings plain;
    plain.count = 12;
    plain.minSpacingMeters = 1.0F;
    plain.seed = 5;
    const ScatterPlan flat = plan_scatter(document, 1, 1, plain);
    check(flat.samples.size() == 12, "12 spots fit on one slab");
    for (const ScatterSample& s : flat.samples)
        check(s.yawRadians == 0.0F && s.scale == 1.0F, "no variation by default");

    ScatterSettings varied = plain;
    varied.yawJitterDegrees = 90.0F;  // +/-45 deg
    varied.minScale = 0.5F;
    varied.maxScale = 1.5F;
    const ScatterPlan plan = plan_scatter(document, 1, 1, varied);
    check(plan.samples.size() == flat.samples.size(), "variation does not change how many spots fit");
    bool moved = false, turned = false, scaledDown = false, scaledUp = false;
    for (std::size_t i = 0; i < plan.samples.size() && i < flat.samples.size(); ++i) {
        const ScatterSample& s = plan.samples[i];
        moved = moved || !near(s.position.x, flat.samples[i].position.x, 0.0F) || !near(s.position.z, flat.samples[i].position.z, 0.0F);
        check(std::abs(s.yawRadians) <= 0.7854F + 1.0e-4F, "turn stays within +/-45 deg");
        check(s.scale >= 0.5F && s.scale <= 1.5F, "scale stays in range");
        turned = turned || std::abs(s.yawRadians) > 0.05F;
        scaledDown = scaledDown || s.scale < 0.9F;
        scaledUp = scaledUp || s.scale > 1.1F;
    }
    check(!moved, "turning variation on never moves a spot");
    check(turned && scaledDown && scaledUp, "copies get a spread of turns and sizes");
    const ScatterPlan again = plan_scatter(document, 1, 1, varied);
    bool same = again.samples.size() == plan.samples.size();
    for (std::size_t i = 0; same && i < plan.samples.size(); ++i)
        same = again.samples[i].yawRadians == plan.samples[i].yawRadians && again.samples[i].scale == plan.samples[i].scale;
    check(same, "variation is deterministic for a seed");

    // Copies: voxel size follows the scale, the copy still stands on its spot, the turn is a
    // rotation about the up axis.
    EditorDocument target = make_scene();
    std::vector<ScatterSample> samples{{{3.0F, 0.25F, 3.0F}, {0, 1, 0}, 0, 0.5F, 2.0F}};
    const std::vector<ScatterSource> sources{{&target, {3}, "Rock"}};
    auto command = std::make_unique<CompoundCommand>("test");
    const ScatterCopiesResult copies = append_scatter_copies(*command, target, 999, samples, sources, false);
    EditorObject group = make_scatter_group(999, 1, {3.0F, 0.25F, 3.0F});
    EditorCommandStack stack;
    check(stack.execute(target, std::make_unique<AddObjectCommand>(std::move(group))).success, "add group");
    check(copies.error.empty() && copies.rootIds.size() == 1, "one copy built");
    check(stack.execute(target, std::move(command)).success, "copies added");
    const EditorObject* copy = copies.rootIds.empty() ? nullptr : target.find_object(copies.rootIds.front());
    check(copy && near(copy->voxelSizeMeters, 0.5F), "scale 2 doubles the voxel size");
    if (copy) {
        const EditorObjectBounds b = object_world_bounds(*copy);
        check(near(b.minimum.y, 0.25F, 0.02F), "a scaled copy still stands on the ground");
        check(near((b.minimum.x + b.maximum.x) * 0.5F, 3.0F, 0.05F) && near((b.minimum.z + b.maximum.z) * 0.5F, 3.0F, 0.05F),
              "a turned, scaled copy stays centred on its spot");
        check(near(b.maximum.y - b.minimum.y, 1.0F, 0.02F), "scale 2: the 0.5 m rock is 1 m tall");
        check(b.maximum.x - b.minimum.x > 1.2F, "turned 0.5 rad, its footprint is wider than 1 m");
        const Quaternion q = copy->transform.rotation;
        check(near(q.x, 0.0F) && near(q.z, 0.0F) && std::abs(q.y) > 0.1F, "the turn is about the up axis only");
    }
}

void test_density_and_multi_surface() {
    const EditorDocument document = make_scene();
    const auto onA = [](EditorObjectId id) { return id == 1; };
    ScatterDabSettings full;
    full.minSpacingMeters = 0.5F;
    full.seed = 9;
    full.sourceCount = 1;
    ScatterDabSettings thin = full;
    thin.density = 0.2F;
    const auto many = plan_scatter_dab(document, {3.0F, 0.25F, 3.0F}, 2.0F, full, {}, onA);
    const auto few = plan_scatter_dab(document, {3.0F, 0.25F, 3.0F}, 2.0F, thin, {}, onA);
    check(!few.empty() && few.size() * 2 < many.size(),
          "density 20% places far fewer copies (" + std::to_string(few.size()) + " vs " + std::to_string(many.size()) + ")");

    // Going over the same spot again does not build up past the density.
    std::vector<Float3> occupied;
    for (const ScatterSample& sample : few) occupied.push_back(sample.position);
    thin.seed = 10;
    const auto second = plan_scatter_dab(document, {3.0F, 0.25F, 3.0F}, 2.0F, thin, occupied, onA);
    check(second.size() <= 1, "a second low-density dab over the same spot adds (almost) nothing, got " +
          std::to_string(second.size()));

    ScatterSettings settings;
    settings.count = 30;
    settings.minSpacingMeters = 0.8F;
    settings.seed = 3;
    const ScatterPlan plan = plan_scatter(document, std::vector<EditorObjectId>{1, 2}, 1, settings);
    check(plan.error.empty(), "two surfaces plan: " + plan.error);
    std::size_t onFirst = 0, onSecond = 0;
    for (const ScatterSample& s : plan.samples) {
        if (s.position.x <= 6.0F) ++onFirst;
        else if (s.position.x >= 20.0F && s.position.x <= 26.0F) ++onSecond;
        check(near(s.position.y, 0.25F), "every spot is on a slab top");
    }
    check(onFirst + onSecond == plan.samples.size(), "no spot lands between the slabs");
    check(onFirst >= 8 && onSecond >= 8, "both surfaces get copies (" + std::to_string(onFirst) + "/" + std::to_string(onSecond) + ")");
    check(plan.missedSurface == 0, "sampling per surface wastes no tries on the gap between them");
    EditorDocument withText = make_scene();
    EditorObject empty(7, "Empty");
    withText.add_object(std::move(empty));
    check(!plan_scatter(withText, std::vector<EditorObjectId>{1, 7}, 1, settings).error.empty(),
          "a surface without voxels is refused with a reason");
}

void test_settings_component_and_save() {
    ScatterSettings settings;
    settings.count = 42;
    settings.minSpacingMeters = 0.75F;
    settings.seed = 0xFFFFFFFFFFFFFFF0ULL;  // does not fit a signed integer
    settings.alignToSurface = true;
    settings.yawJitterDegrees = 360.0F;
    settings.minScale = 0.7F;
    settings.maxScale = 1.3F;
    settings.brushRadiusMeters = 3.5F;
    settings.brushDensity = 0.4F;
    const EditorObject group = make_scatter_group(50, 3, {}, settings);
    const auto read = read_scatter_settings(group);
    check(read && *read == sanitize_scatter_settings(settings), "settings round-trip through the group component");

    // Saved with the scene.
    EditorDocument document = make_scene();
    EditorCommandStack stack;
    check(stack.execute(document, std::make_unique<AddObjectCommand>(make_scatter_group(document.allocate_object_id(), 0, {}, settings))).success,
          "add group");
    const auto groups = scatter_groups(document);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "dve_scatter_settings_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto saved = document.save_transactional(folder / "scene.dvescene");
    check(saved.success, "scene saves: " + saved.error);
    std::string error;
    auto loaded = EditorDocument::load(folder / "scene.dvescene", &error);
    check(loaded.has_value(), "scene loads: " + error);
    if (loaded && !groups.empty()) {
        const EditorObject* again = loaded->find_object(groups.front());
        const auto stored = again ? read_scatter_settings(*again) : std::nullopt;
        check(stored && *stored == sanitize_scatter_settings(settings), "settings survive save and load");
    }
    std::filesystem::remove_all(folder);

    // Updating: nothing when equal, only the differing properties otherwise, and undoable.
    if (!groups.empty()) {
        const EditorObject& stored = *document.find_object(groups.front());
        CompoundCommand same("same");
        append_scatter_settings_update(same, stored, settings);
        check(same.empty(), "no commands when the group already has these settings");
        ScatterSettings changed = settings;
        changed.minScale = 0.9F;
        auto update = std::make_unique<CompoundCommand>("update");
        append_scatter_settings_update(*update, stored, changed);
        check(stack.execute(document, std::move(update)).success, "update executes");
        check(read_scatter_settings(*document.find_object(groups.front()))->minScale == 0.9F, "group has the new settings");
        check(stack.undo(document).success && near(read_scatter_settings(*document.find_object(groups.front()))->minScale, 0.7F),
              "undo restores the old settings");
    }
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    controller->resize(1280, 720);
    return controller;
}

void test_inspector_panel() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(3);
    check(controller.layout().scatterSettingRows.empty(), "no scatter panel normally");
    controller.set_active_tool(EditorToolId::ScatterBrush);
    const auto& rows = controller.layout().scatterSettingRows;
    check(rows.size() == NativeEditorController::kScatterSettingFieldCount, "the brush shows one row per setting");
    check(controller.layout().inspectorFields.empty() && controller.layout().inspectorToggles.empty(),
          "object fields and flag toggles are not clickable under the panel");
    for (const UiRect& row : rows) check(row.width > 0 && row.y + row.height <= controller.layout().inspector.y + controller.layout().inspector.height,
                                         "every row fits the inspector at 1280x720");
    const auto row_of = [&](NativeEditorController::ScatterSettingField field) {
        return controller.layout().scatterSettingRows[static_cast<std::size_t>(field)];
    };
    const auto click = [&](UiRect r) {
        controller.pointer_down(PointerButton::Primary, r.x + 20, r.y + r.height / 2);
        controller.pointer_up(PointerButton::Primary, r.x + 20, r.y + r.height / 2);
    };
    // Type a spacing.
    click(row_of(NativeEditorController::ScatterSettingField::Spacing));
    check(controller.text_edit().kind == TextEditKind::ScatterSetting, "clicking a row starts typing");
    controller.text_input("1.75");
    controller.key_down("return", false, false, false);
    check(near(controller.scatter_settings().minSpacingMeters, 1.75F), "typed spacing applies");
    // Turn is typed as +/- half the range.
    click(row_of(NativeEditorController::ScatterSettingField::YawJitter));
    controller.text_input("180");
    controller.key_down("return", false, false, false);
    check(near(controller.scatter_settings().yawJitterDegrees, 360.0F), "+/-180 means any direction");
    // A bad value keeps the field open with an error.
    click(row_of(NativeEditorController::ScatterSettingField::MinScale));
    controller.text_input("big");
    controller.key_down("return", false, false, false);
    check(controller.text_edit().kind == TextEditKind::ScatterSetting && !controller.text_edit().error.empty(),
          "a bad value is refused and stays open");
    controller.key_down("escape", false, false, false);
    // Min above max pushes max up.
    check(controller.set_scatter_setting(NativeEditorController::ScatterSettingField::MinScale, "1.4"), "set min scale");
    check(near(controller.scatter_settings().maxScale, 1.4F), "max follows min");
    // Align toggles on click; the wheel nudges.
    const bool align = controller.scatter_settings().alignToSurface;
    click(row_of(NativeEditorController::ScatterSettingField::Align));
    check(controller.scatter_settings().alignToSurface != align, "Align toggles on click");
    const UiRect density = row_of(NativeEditorController::ScatterSettingField::BrushDensity);
    controller.pointer_wheel(-2.0F, density.x + 20, density.y + 5);
    check(near(controller.scatter_settings().brushDensity, 0.95F), "wheel down nudges density by 5%");
    const UiRect radius = row_of(NativeEditorController::ScatterSettingField::BrushRadius);
    const float before = controller.scatter_settings().brushRadiusMeters;
    controller.pointer_wheel(1.0F, radius.x + 20, radius.y + 5);
    check(controller.scatter_settings().brushRadiusMeters > before && controller.scatter_brush_view().radius > before,
          "the radius row and the brush ring are the same setting");

    struct Texts final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override {}
        void text(int, int, std::string_view value, EditorColor) const override { all.emplace_back(value); }
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        [[nodiscard]] bool has(std::string_view needle) const {
            return std::any_of(all.begin(), all.end(), [&](const std::string& t) { return t.find(needle) != std::string::npos; });
        }
        mutable std::vector<std::string> all;
    } canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Scatter brush settings") && canvas.has("Random turn") && canvas.has("+/-180 deg") &&
              canvas.has("Brush density"),
          "the inspector draws the scatter settings");
    check(!canvas.has("Visible"), "object flag toggles are hidden under the panel");

    controller.set_active_tool(EditorToolId::Select);
    check(controller.layout().scatterSettingRows.empty() && !controller.layout().inspectorToggles.empty(),
          "leaving the brush brings the object inspector back");
}

void test_two_step_sources_and_saving() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.workspace().select_object(3);
    check(controller.dispatch_action("scatter.set_sources"), "Set Scatter Sources with the rock selected");
    check(controller.scatter_sources_pinned() && controller.pinned_scatter_sources() == std::vector<EditorObjectId>{3},
          "the rock is the source");
    controller.workspace().select_object(1);
    controller.workspace().add_to_selection(2);
    check(controller.scatter_disabled_reason().empty(), "two surfaces selected: ready");
    ScatterSettings settings = controller.scatter_settings();
    settings.count = 24;
    settings.minSpacingMeters = 0.8F;
    settings.yawJitterDegrees = 360.0F;
    settings.minScale = 0.8F;
    settings.maxScale = 1.2F;
    controller.set_scatter_settings(settings);
    check(controller.dispatch_action("create.scatter"), "Scatter Objects opens");
    check(controller.scatter_targets() == std::vector<EditorObjectId>{1, 2}, "both selected slabs are surfaces");
    check(controller.layout().scatterSettingRows.size() == NativeEditorController::kScatterSettingFieldCount,
          "the fill preview shows the settings panel too");
    const auto lines = controller.scatter_preview_lines();
    check(lines.size() > 2 && lines[1].starts_with("Surfaces (2)") && lines[2].find("(set sources)") != std::string::npos,
          "the preview names both surfaces and says the sources are set");
    check(controller.dispatch_action("scatter.commit"), "commit");
    const auto groups = scatter_groups(document);
    check(groups.size() == 1, "one group");
    std::size_t onA = 0, onB = 0;
    for (const EditorObjectId child : groups.empty() ? std::vector<EditorObjectId>{} : document.children_of(groups.front())) {
        const EditorObject* copy = document.find_object(child);
        const EditorObjectBounds b = object_world_bounds(*copy);
        const float x = (b.minimum.x + b.maximum.x) * 0.5F;
        (x < 10.0F ? onA : onB) += 1;
        check(copy->voxelSizeMeters >= 0.2F - 1e-4F && copy->voxelSizeMeters <= 0.3F + 1e-4F, "copy voxel size within 0.8-1.2x");
    }
    check(onA > 0 && onB > 0, "copies on both surfaces");
    const auto stored = groups.empty() ? std::nullopt : read_scatter_settings(*document.find_object(groups.front()));
    check(stored && stored->count == 24 && near(stored->minScale, 0.8F) && near(stored->yawJitterDegrees, 360.0F),
          "the group stores the settings that made it");
    controller.clear_scatter_sources();
    check(!controller.scatter_sources_pinned(), "sources cleared");

    // Settings come back: change them, then pick the brush with the group selected.
    ScatterSettings other = controller.scatter_settings();
    other.minScale = other.maxScale = 2.0F;
    controller.set_scatter_settings(other);
    controller.workspace().select_object(groups.front());
    controller.workspace().add_to_selection(3);  // paint rocks
    controller.workspace().select_object(groups.front());
    controller.dispatch_action("scatter.brush");
    check(controller.active_tool() == EditorToolId::ScatterBrush, "Create > Scatter Brush picks the tool");
    check(near(controller.scatter_settings().minScale, 0.8F) && controller.scatter_settings().count == 24,
          "picking the brush with a scatter group active takes that group's settings");

    // A fresh editor on the same scene continues from the newest group.
    auto fresh = std::make_unique<NativeEditorController>(EditorWorkspace(clone_editor_document(document)));
    fresh->resize(1280, 720);
    fresh->workspace().select_object(3);
    fresh->workspace().add_to_selection(1);
    check(fresh->dispatch_action("create.scatter"), "fill in the reopened scene");
    check(fresh->scatter_settings().count == 24 && near(fresh->scatter_settings().maxScale, 1.2F),
          "the reopened scene starts from its newest scatter group's settings");
}

void test_shortcut_and_accessibility() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(controller.tool_shortcut_text(EditorToolId::ScatterBrush) == "Y", "Scatter brush is Y");
    const UiRect viewport = controller.layout().viewport;
    controller.pointer_down(PointerButton::Primary, viewport.x + 5, viewport.y + 40);
    controller.pointer_up(PointerButton::Primary, viewport.x + 5, viewport.y + 40);
    controller.key_down("y", false, false, false);
    check(controller.active_tool() == EditorToolId::ScatterBrush, "Y in the viewport picks the brush");
    const MenuAction* item = std::as_const(controller.workspace().menus()).find("scatter.brush");
    check(item && item->menu == "Create", "Scatter Brush is in the Create menu (and the Command Center)");
    check(std::as_const(controller.workspace().menus()).find("scatter.set_sources") != nullptr, "Set Scatter Sources menu item");
    const AccessibilityNode tree = build_editor_accessibility_tree(controller);
    bool labelled = false;
    for (const AccessibilityNode& node : tree.children)
        if (node.id == "toolbar")
            for (const AccessibilityNode& button : node.children)
                if (button.label == "Scatter brush") labelled = button.shortcut == "Y";
    check(labelled, "accessibility gives the brush button its real shortcut, not its position");
}

} // namespace

int main() {
    test_variation();
    test_density_and_multi_surface();
    test_settings_component_and_save();
    test_inspector_panel();
    test_two_step_sources_and_saving();
    test_shortcut_and_accessibility();
    if (g_failures != 0) {
        std::printf("dve_editor_scatter_settings_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_scatter_settings_tests: PASS\n");
    return 0;
}
