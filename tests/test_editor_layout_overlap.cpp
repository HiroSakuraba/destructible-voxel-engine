// Layout regression tests: no overlapping widgets and correct hit-testing for
//   * the synth Mod Matrix macro strip vs parameter grid rows 8-9 (and the
//     Performance arp strip, which had the same fixed offset), and
//   * the inspector flag toggles vs the bottom dock,
// at 1280x719 and other common windows for every UI zoom 100-200 %.
#include "dve/editor_native.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool intersects(const UiRect& a, const UiRect& b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) return false;
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
bool inside(const UiRect& inner, const UiRect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}
std::string rect_text(const UiRect& r) {
    return "{" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.width) + "," +
           std::to_string(r.height) + "}";
}
void click(NativeEditorController& controller, UiRect rect) {
    controller.pointer_down(PointerButton::Primary, rect.x + rect.width / 2, rect.y + rect.height / 2);
    controller.pointer_up(PointerButton::Primary, rect.x + rect.width / 2, rect.y + rect.height / 2);
}

struct Case { int windowWidth; int windowHeight; float zoom; int logicalWidth; int logicalHeight; };

std::vector<Case> cases() {
    std::vector<Case> result;
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 719}, {1280, 720}, {1366, 768}, {1920, 1080}, {2560, 1440}}) {
        for (float zoom : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F}) {
            const float effective = effective_ui_zoom(zoom, w, h);
            result.push_back({w, h, zoom, ui_zoom_logical_extent(w, effective), ui_zoom_logical_extent(h, effective)});
        }
    }
    return result;
}

void test_synth_grid_and_strips(const Case& c) {
    const std::string tag = std::to_string(c.windowWidth) + "x" + std::to_string(c.windowHeight) + "@" +
                            std::to_string(static_cast<int>(c.zoom * 100)) + "% (logical " +
                            std::to_string(c.logicalWidth) + "x" + std::to_string(c.logicalHeight) + ") ";
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(c.logicalWidth, c.logicalHeight);
    if (!controller.synth_panel().open()) (void)controller.dispatch_action("window.toggle_synth");
    const SynthPanelLayout panel = controller.synth_panel().layout();
    for (std::size_t m = 0; m < panel.macroRows.size(); ++m) {
        check(inside(panel.macroRows[m], panel.panel), tag + "macro row outside panel");
        for (std::size_t p = 0; p < panel.parameterRows.size(); ++p)
            check(!intersects(panel.macroRows[m], panel.parameterRows[p]),
                  tag + "macro " + std::to_string(m) + rect_text(panel.macroRows[m]) + " overlaps parameter row " +
                  std::to_string(p) + rect_text(panel.parameterRows[p]));
        check(inside(panel.macroDownButtons[m], panel.macroRows[m]) && inside(panel.macroUpButtons[m], panel.macroRows[m]),
              tag + "macro buttons outside macro row");
    }
    for (std::size_t s = 0; s < panel.arpeggiatorStepButtons.size(); ++s)
        for (std::size_t p = 0; p < panel.parameterRows.size(); ++p)
            check(!intersects(panel.arpeggiatorStepButtons[s], panel.parameterRows[p]),
                  tag + "arp step button overlaps parameter row " + std::to_string(p));
    for (std::size_t p = 0; p < panel.parameterRows.size(); ++p) {
        check(inside(panel.parameterDownButtons[p], panel.parameterRows[p]) &&
              inside(panel.parameterUpButtons[p], panel.parameterRows[p]) &&
              inside(panel.parameterToggleButtons[p], panel.parameterRows[p]),
              tag + "parameter row " + std::to_string(p) + " buttons spill outside their row");
        for (std::size_t q = p + 1; q < panel.parameterRows.size(); ++q)
            check(!intersects(panel.parameterRows[p], panel.parameterRows[q]), tag + "parameter rows overlap");
    }
    // The macro strip must clear the piano whenever the logical canvas is at least
    // what 1280x719 produces at its maximum zoom (1024x575).
    if (c.logicalHeight >= 575)
        for (const UiRect& macro : panel.macroRows)
            check(macro.y + macro.height <= panel.meterArea.y && panel.meterArea.y + panel.meterArea.height <= panel.pianoKeys[0].y,
                  tag + "macro strip overlaps the voice meter / piano");
    // Performance (grid + arp steps + 5 step rows) fits from a 686 px logical height up
    // (1280x719 @100 %, 1920x1080 @150 %, 2560x1440 @200 %).
    if (c.logicalHeight >= 686) {
        for (const UiRect& row : panel.arpeggiatorStepRows)
            check(row.y + row.height <= panel.meterArea.y, tag + "arp step rows overlap the voice meter");
        for (const UiRect& button : panel.arpeggiatorStepButtons)
            for (const UiRect& row : panel.arpeggiatorStepRows)
                check(!intersects(button, row), tag + "arp step button overlaps a step row");
    }

    // Hit-testing on the Mod Matrix page.
    click(controller, panel.tabButtons[2]);
    if (controller.synth_panel().page() != SynthPanelPage::Modulation) {
        check(false, tag + "could not open the Modulation page");
        return;
    }
    auto& synth = controller.synthesizer();
    const auto before = synth.preset();
    click(controller, panel.macroUpButtons[0]);
    const auto afterMacro = synth.preset();
    check(afterMacro.macros.values[0] > before.macros.values[0], tag + "macro 1 '+' did not change macro 1");
    check(afterMacro.lfos[0].beatsPerCycle == before.lfos[0].beatsPerCycle &&
          afterMacro.lfos[1].enabled == before.lfos[1].enabled, tag + "macro click edited an LFO parameter");
    click(controller, panel.parameterToggleButtons[9]);  // "LFO 2" enable (row 9)
    const auto afterRow9 = synth.preset();
    check(afterRow9.lfos[1].enabled != afterMacro.lfos[1].enabled, tag + "row 9 toggle did not toggle LFO 2");
    check(afterRow9.macros.values == afterMacro.macros.values, tag + "row 9 click edited a macro");
    click(controller, panel.parameterUpButtons[8]);      // "LFO 1 beats/cycle" (row 8)
    const auto afterRow8 = synth.preset();
    check(afterRow8.lfos[0].beatsPerCycle != afterRow9.lfos[0].beatsPerCycle, tag + "row 8 '+' did not change beats/cycle");
    check(afterRow8.macros.values == afterRow9.macros.values, tag + "row 8 click edited a macro");
}

void test_inspector_toggles(const Case& c) {
    const std::string tag = std::to_string(c.windowWidth) + "x" + std::to_string(c.windowHeight) + "@" +
                            std::to_string(static_cast<int>(c.zoom * 100)) + "% ";
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.workspace().select_object(1004);
    controller.resize(c.logicalWidth, c.logicalHeight);
    const NativeEditorLayout& layout = controller.layout();
    check(layout.inspectorToggles.size() == 5U, tag + "expected 5 inspector toggles, got " +
                                                   std::to_string(layout.inspectorToggles.size()));
    for (std::size_t i = 0; i < layout.inspectorToggles.size(); ++i) {
        const UiRect& toggle = layout.inspectorToggles[i];
        check(inside(toggle, layout.inspector), tag + "toggle " + std::to_string(i) + rect_text(toggle) +
                                                    " outside inspector " + rect_text(layout.inspector));
        check(!intersects(toggle, layout.bottomPanel), tag + "toggle " + std::to_string(i) + " spills into the bottom dock");
        check(toggle.y >= layout.inspectorContentClipY, tag + "toggle drawn over inspector detail text");
        for (std::size_t j = i + 1; j < layout.inspectorToggles.size(); ++j)
            check(!intersects(toggle, layout.inspectorToggles[j]), tag + "inspector toggles overlap");
        for (const UiRect& field : layout.inspectorFields)
            check(!intersects(toggle, field), tag + "toggle overlaps an inspector field");
    }
    check(layout.inspectorContentClipY <= layout.inspector.y + layout.inspector.height, tag + "clip below inspector");
    // Hit-test: "Locked" (index 1) toggles locked, and a click in the dock just below the
    // inspector does not toggle any flag.
    if (layout.inspectorToggles.size() >= 2U) {
        const bool lockedBefore = controller.workspace().document().find_object(1004)->flags.locked;
        click(controller, layout.inspectorToggles[1]);
        check(controller.workspace().document().find_object(1004)->flags.locked != lockedBefore,
              tag + "clicking 'Locked' did not toggle locked");
        const auto flagsBefore = controller.workspace().document().find_object(1004)->flags;
        const int dockY = layout.bottomPanel.y + layout.bottomPanel.height - 12;
        controller.pointer_down(PointerButton::Primary, layout.inspector.x + 40, dockY);
        controller.pointer_up(PointerButton::Primary, layout.inspector.x + 40, dockY);
        check(controller.workspace().document().find_object(1004)->flags == flagsBefore,
              tag + "click in the bottom dock toggled an inspector flag");
    }
}

} // namespace

int main() {
    for (const Case& c : cases()) {
        test_synth_grid_and_strips(c);
        test_inspector_toggles(c);
    }
    if (g_failures == 0) std::printf("editor layout overlap tests passed (%zu window/zoom cases)\n", cases().size());
    return g_failures == 0 ? 0 : 1;
}
