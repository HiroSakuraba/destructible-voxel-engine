// Layout regression tests: no overlapping widgets and correct hit-testing for
//   * the synth grid pages (Mod Matrix macro strip vs grid rows 8-9, the
//     Performance arp steps + step rows, which scroll above the voice meter
//     down to the 640x480 minimum logical layout), and
//   * the inspector flag toggles vs the bottom dock,
// at 1280x719 and other common windows for every UI zoom 100-200 %.
#include "dve/editor_native.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
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
    // 1280x960 @200 % is the 640x480 minimum logical layout; 1280x719 @125 % is 1024x575.
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 719}, {1280, 720}, {1366, 768}, {1920, 1080},
                                                         {2560, 1440}, {1280, 960}, {800, 600}, {640, 480}}) {
        for (float zoom : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F}) {
            const float effective = effective_ui_zoom(zoom, w, h);
            result.push_back({w, h, zoom, ui_zoom_logical_extent(w, effective), ui_zoom_logical_extent(h, effective)});
        }
    }
    return result;
}

std::string case_tag(const Case& c) {
    return std::to_string(c.windowWidth) + "x" + std::to_string(c.windowHeight) + "@" +
           std::to_string(static_cast<int>(c.zoom * 100)) + "% (logical " + std::to_string(c.logicalWidth) + "x" +
           std::to_string(c.logicalHeight) + ") ";
}

bool visible(const UiRect& r) { return r.width > 0 && r.height > 0; }

// Everything a grid page shows at the current scroll position: parameter rows,
// the page's strip (macros / arp step buttons / arp step rows), each visible
// element inside the grid viewport, above the voice meter, and not overlapping
// any other element. Returns the visible elements for coverage checks.
struct GridElement { std::string name; UiRect rect; };
std::vector<GridElement> check_grid_page(const SynthPanelLayout& panel, SynthPanelPage page, const std::string& tag) {
    std::vector<GridElement> elements;
    auto add = [&](const std::string& name, const UiRect& rect) { if (visible(rect)) elements.push_back({name, rect}); };
    for (std::size_t p = 0; p < panel.parameterRows.size(); ++p) {
        add("parameter row " + std::to_string(p), panel.parameterRows[p]);
        if (!visible(panel.parameterRows[p])) {
            check(!visible(panel.parameterDownButtons[p]) && !visible(panel.parameterUpButtons[p]) &&
                  !visible(panel.parameterToggleButtons[p]), tag + "hidden parameter row keeps clickable buttons");
            continue;
        }
        check(inside(panel.parameterDownButtons[p], panel.parameterRows[p]) &&
              inside(panel.parameterUpButtons[p], panel.parameterRows[p]) &&
              inside(panel.parameterToggleButtons[p], panel.parameterRows[p]),
              tag + "parameter row " + std::to_string(p) + " buttons spill outside their row");
    }
    if (page == SynthPanelPage::Modulation)
        for (std::size_t m = 0; m < panel.macroRows.size(); ++m) {
            add("macro " + std::to_string(m), panel.macroRows[m]);
            if (visible(panel.macroRows[m]))
                check(inside(panel.macroDownButtons[m], panel.macroRows[m]) && inside(panel.macroUpButtons[m], panel.macroRows[m]),
                      tag + "macro buttons outside macro row");
        }
    else
        for (const UiRect& macro : panel.macroRows) check(!visible(macro), tag + "macro strip laid out off its page");
    if (page == SynthPanelPage::Performance) {
        for (std::size_t s = 0; s < panel.arpeggiatorStepButtons.size(); ++s)
            add("arp step button " + std::to_string(s), panel.arpeggiatorStepButtons[s]);
        for (std::size_t r = 0; r < panel.arpeggiatorStepRows.size(); ++r) {
            add("arp step row " + std::to_string(r), panel.arpeggiatorStepRows[r]);
            if (!visible(panel.arpeggiatorStepRows[r])) continue;
            check(inside(panel.arpeggiatorStepDownButtons[r], panel.arpeggiatorStepRows[r]) &&
                  inside(panel.arpeggiatorStepUpButtons[r], panel.arpeggiatorStepRows[r]) &&
                  inside(panel.arpeggiatorStepToggleButtons[r], panel.arpeggiatorStepRows[r]),
                  tag + "arp step row " + std::to_string(r) + " buttons spill outside their row");
        }
    } else {
        for (const UiRect& row : panel.arpeggiatorStepRows) check(!visible(row), tag + "arp rows laid out off their page");
    }
    check(visible(panel.gridViewport), tag + "grid page without a viewport");
    for (const GridElement& element : elements) {
        check(inside(element.rect, panel.gridViewport),
              tag + element.name + rect_text(element.rect) + " outside the grid viewport " + rect_text(panel.gridViewport));
        check(element.rect.y + element.rect.height <= panel.meterArea.y,
              tag + element.name + rect_text(element.rect) + " runs into the voice meter " + rect_text(panel.meterArea));
        check(!intersects(element.rect, panel.pianoArea), tag + element.name + " overlaps the piano");
        if (visible(panel.gridScrollTrack))
            check(!intersects(element.rect, panel.gridScrollTrack), tag + element.name + " under the scrollbar");
    }
    for (std::size_t a = 0; a < elements.size(); ++a)
        for (std::size_t b = a + 1; b < elements.size(); ++b)
            check(!intersects(elements[a].rect, elements[b].rect),
                  tag + elements[a].name + rect_text(elements[a].rect) + " overlaps " + elements[b].name +
                  rect_text(elements[b].rect));
    return elements;
}

void test_synth_grid_and_strips(const Case& c) {
    const std::string tag = case_tag(c);
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(c.logicalWidth, c.logicalHeight);
    if (!controller.synth_panel().open()) (void)controller.dispatch_action("window.toggle_synth");
    EditorSynthPanel& synthPanel = controller.synth_panel();
    {
        const SynthPanelLayout& panel = synthPanel.layout();
        check(inside(panel.meterArea, panel.panel) && inside(panel.pianoArea, panel.panel), tag + "meter/piano outside panel");
        check(panel.meterArea.y + panel.meterArea.height <= panel.pianoArea.y, tag + "voice meter overlaps the piano");
        for (std::size_t t = 0; t < panel.tabButtons.size(); ++t) {
            check(inside(panel.tabButtons[t], panel.panel), tag + "page tab " + std::to_string(t) + " runs off the panel");
            if (t > 0) check(!intersects(panel.tabButtons[t - 1], panel.tabButtons[t]), tag + "page tabs overlap");
            check(!intersects(panel.tabButtons[t], panel.midiThruButton), tag + "page tab overlaps MIDI THRU");
        }
        check(inside(panel.keyboardKeysButton, panel.panel) && !intersects(panel.keyboardKeysButton, panel.pianoArea),
              tag + "keyboard size button misplaced");
    }
    for (const SynthPanelPage page : {SynthPanelPage::FilterEnvelope, SynthPanelPage::Modulation,
                                      SynthPanelPage::Performance, SynthPanelPage::Expression,
                                      SynthPanelPage::Generative}) {
        const std::string pageTag = tag + "page " + std::to_string(static_cast<int>(page)) + ": ";
        click(controller, synthPanel.layout().tabButtons[static_cast<std::size_t>(page)]);
        if (synthPanel.page() != page) { check(false, pageTag + "tab did not open the page"); continue; }
        check(synthPanel.grid_scroll() == 0, pageTag + "page opens scrolled");
        // Performance (grid + arp steps + 5 step rows) fits without scrolling from a 686 px
        // logical height (1280x719 @100 %, 1920x1080 @150 %, 2560x1440 @200 %); the Mod
        // Matrix strip from 575 px (1280x719 @125 %). Smaller canvases scroll.
        if (page == SynthPanelPage::Performance && c.logicalHeight >= 686)
            check(synthPanel.grid_max_scroll() == 0, pageTag + "Performance scrolls although it fits");
        if (page == SynthPanelPage::Modulation && c.logicalHeight >= 575)
            check(synthPanel.grid_max_scroll() == 0, pageTag + "Mod Matrix scrolls although it fits");
        std::vector<std::string> seen;
        for (int step = 0; step <= synthPanel.grid_max_scroll(); ++step) {
            const std::string scrollTag = pageTag + "scroll " + std::to_string(synthPanel.grid_scroll()) + ": ";
            const SynthPanelLayout& panel = synthPanel.layout();
            check(visible(panel.gridScrollTrack) == (synthPanel.grid_max_scroll() > 0), scrollTag + "scrollbar state");
            if (visible(panel.gridScrollTrack))
                check(inside(panel.gridScrollThumb, panel.gridScrollTrack), scrollTag + "scroll thumb outside the track");
            for (const GridElement& element : check_grid_page(panel, page, scrollTag)) seen.push_back(element.name);
            if (step < synthPanel.grid_max_scroll()) (void)controller.synth_panel().scroll_grid(1);
        }
        // Every row / strip element is reachable at some scroll position.
        const std::size_t expected = 30U + (page == SynthPanelPage::Modulation ? 4U : 0U) +
                                     (page == SynthPanelPage::Performance ? 16U + 18U : 0U);
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        check(seen.size() == expected, pageTag + "only " + std::to_string(seen.size()) + " of " +
                                           std::to_string(expected) + " rows are reachable by scrolling");
        // The wheel over the grid scrolls back to the top.
        const UiRect viewport = synthPanel.layout().gridViewport;
        controller.pointer_wheel(100.0F, viewport.x + 20, viewport.y + 10);
        check(synthPanel.grid_scroll() == 0, pageTag + "wheel up did not return to the top");
        if (synthPanel.grid_max_scroll() > 0) {
            controller.pointer_wheel(-1.0F, viewport.x + 20, viewport.y + 10);
            check(synthPanel.grid_scroll() == 1, pageTag + "wheel down did not scroll one line");
            controller.pointer_wheel(1.0F, viewport.x + 20, viewport.y + 10);
        }
    }

    // Hit-testing on the Mod Matrix page (scrolling to each target first).
    click(controller, synthPanel.layout().tabButtons[2]);
    if (synthPanel.page() != SynthPanelPage::Modulation) {
        check(false, tag + "could not open the Modulation page");
        return;
    }
    EditorSynthPanel& panelRef = synthPanel;
    auto reveal = [&](auto&& rectOf) {
        (void)panelRef.scroll_grid(-100);
        while (!visible(rectOf()) && panelRef.scroll_grid(1)) {}
        return rectOf();
    };
    auto& synth = controller.synthesizer();
    const auto before = synth.preset();
    click(controller, reveal([&] { return synthPanel.layout().macroUpButtons[0]; }));
    const auto afterMacro = synth.preset();
    check(afterMacro.macros.values[0] > before.macros.values[0], tag + "macro 1 '+' did not change macro 1");
    check(afterMacro.lfos[0].beatsPerCycle == before.lfos[0].beatsPerCycle &&
          afterMacro.lfos[1].enabled == before.lfos[1].enabled, tag + "macro click edited an LFO parameter");
    click(controller, reveal([&] { return synthPanel.layout().parameterToggleButtons[9]; }));  // "LFO 2" enable (row 9)
    const auto afterRow9 = synth.preset();
    check(afterRow9.lfos[1].enabled != afterMacro.lfos[1].enabled, tag + "row 9 toggle did not toggle LFO 2");
    check(afterRow9.macros.values == afterMacro.macros.values, tag + "row 9 click edited a macro");
    click(controller, reveal([&] { return synthPanel.layout().parameterUpButtons[8]; }));      // "LFO 1 beats/cycle" (row 8)
    const auto afterRow8 = synth.preset();
    check(afterRow8.lfos[0].beatsPerCycle != afterRow9.lfos[0].beatsPerCycle, tag + "row 8 '+' did not change beats/cycle");
    check(afterRow8.macros.values == afterRow9.macros.values, tag + "row 8 click edited a macro");

    // Performance: the last arp step row (step property 17, "Macro 4") is editable
    // at every size, and a click on the scrollbar track pages the grid.
    click(controller, synthPanel.layout().tabButtons[3]);
    const float macro4Before = synth.preset().arpeggiator.steps[0].macro4;
    click(controller, reveal([&] { return synthPanel.layout().arpeggiatorStepUpButtons[17]; }));
    check(synth.preset().arpeggiator.steps[0].macro4 != macro4Before, tag + "arp step 'Macro 4' + unreachable");
    if (synthPanel.grid_max_scroll() > 0) {
        (void)panelRef.scroll_grid(-100);
        const UiRect track = synthPanel.layout().gridScrollTrack;
        click(controller, {track.x, track.y + track.height - 4, track.width, 2});
        check(synthPanel.grid_scroll() > 0, tag + "scrollbar track click did not page down");
    }
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
