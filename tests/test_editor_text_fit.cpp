// Value-cell text fitting: no text drawn in the synth, chiptune or settings
// panels runs past its cell. Long values are elided with an ellipsis, and the
// full value is shown in a tooltip on hover (or focus for the selected
// settings row / tracker cursor cell).
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
std::string rect_text(const UiRect& r) {
    return "{" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.width) + "," +
           std::to_string(r.height) + "}";
}

int code_points(std::string_view value) {
    int count = 0;
    for (const char c : value) if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;
    return count;
}

struct DrawnText { int x; int y; std::string value; };
// 7 px per code point, like the 6x13-ish fallback font.
class MeasuringCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int x, int y, std::string_view value, EditorColor) const override { texts.push_back({x, y, std::string(value)}); }
    [[nodiscard]] int text_width(std::string_view value) const override { return code_points(value) * 7; }
    [[nodiscard]] bool drew(std::string_view value) const {
        return std::any_of(texts.begin(), texts.end(), [&](const DrawnText& t) { return t.value == value; });
    }
    mutable std::vector<DrawnText> texts;
};

void test_elide_helper() {
    MeasuringCanvas canvas;
    check(elide_text_to_width(canvas, "Triangle", 56) == "Triangle", "text that fits is unchanged");
    const std::string elided = elide_text_to_width(canvas, "Triangle", 45);
    check(elided == "Trian\u2026" && canvas.text_width(elided) <= 45, "Triangle elides to 'Trian…', got '" + elided + "'");
    const std::string live = elide_text_to_width(canvas, "0.00 | live 0.00", 50);
    check(canvas.text_width(live) <= 50 && live.ends_with("\u2026") && live.starts_with("0.00"),
          "live value elides, got '" + live + "'");
    check(elide_text_to_width(canvas, "ab cd", 28) == "ab\u2026", "no space before the ellipsis");
    const std::string utf = elide_text_to_width(canvas, "Gr\u00fc\u00dfe Welt", 35);
    check(utf == "Gr\u00fc\u00df\u2026", "UTF-8 is cut on code point boundaries, got '" + utf + "'");
    check(elide_text_to_width(canvas, "Anything", 3).empty(), "nothing fits -> empty");
}

struct Checked { int records = 0; int elided = 0; };

// Every fitted draw stays inside its cell.
Checked check_records(const std::vector<FittedTextRecord>& records, const IEditorCanvas& canvas, const std::string& tag) {
    Checked result;
    for (const FittedTextRecord& record : records) {
        ++result.records;
        if (record.elided) ++result.elided;
        if (record.drawn.empty()) continue;  // nothing fits (e.g. a cell outside a very narrow panel): nothing drawn
        const int right = record.x + canvas.text_width(record.drawn);
        check(record.x >= record.cell.x && right <= record.cell.x + record.cell.width,
              tag + "'" + record.drawn + "' at x=" + std::to_string(record.x) + " (right " + std::to_string(right) +
              ") exceeds its cell " + rect_text(record.cell));
        if (!record.elided) check(record.drawn == record.full, tag + "unelided text changed");
        else check(record.drawn.empty() || record.drawn.ends_with(std::string(canvas.ellipsis())),
                   tag + "elided text without ellipsis: '" + record.drawn + "'");
    }
    return result;
}

// Independent geometric check on the raw draws for the grid pages: labels end
// before the row's first button, values end before '+', and toggle labels stay
// inside the toggle.
void check_grid_geometry(const SynthPanelLayout& panel, const std::vector<FittedTextRecord>& records,
                         const IEditorCanvas& canvas, const std::string& tag) {
    for (const FittedTextRecord& record : records) {
        const DrawnText text{record.x, record.y, record.drawn};
        const int left = text.x;
        const int right = text.x + canvas.text_width(text.value);
        const int probeY = text.y - 4;
        for (std::size_t i = 0; i < panel.parameterRows.size(); ++i) {
            const UiRect& row = panel.parameterRows[i];
            if (!row.contains(left, probeY)) continue;
            const UiRect& down = panel.parameterDownButtons[i];
            const UiRect& up = panel.parameterUpButtons[i];
            const UiRect& toggle = panel.parameterToggleButtons[i];
            if (toggle.contains(left, probeY) && !up.contains(left, probeY))
                check(right <= toggle.x + toggle.width, tag + "toggle text '" + text.value + "' spills out of its button");
            else if (left >= down.x + down.width && left < up.x)
                check(right <= up.x, tag + "value '" + text.value + "' runs into '+' of row " + std::to_string(i));
            else if (left < std::min(down.x, toggle.x))
                check(right <= std::min(down.x, toggle.x), tag + "label '" + text.value + "' runs into row " +
                                                            std::to_string(i) + "'s buttons");
            check(right <= row.x + row.width, tag + "text '" + text.value + "' leaves parameter row " + std::to_string(i));
        }
    }
}

void test_synth_pages() {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 719}, {1024, 575}, {640, 480}}) {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.resize(w, h);
        (void)controller.dispatch_action("window.toggle_synth");
        int elidedTotal = 0;
        for (int page = 0; page < static_cast<int>(kSynthPanelPageCount); ++page) {
            controller.synth_panel().set_page(static_cast<SynthPanelPage>(page));
            const std::string tag = std::to_string(w) + "x" + std::to_string(h) + " synth page " + std::to_string(page) + ": ";
            for (int scroll = 0; scroll <= controller.synth_panel().grid_max_scroll(); ++scroll) {
                std::vector<FittedTextRecord> records;
                CellFitCanvas::set_record_sink(&records);
                MeasuringCanvas canvas;
                render_native_editor(canvas, controller, w, h);
                CellFitCanvas::set_record_sink(nullptr);
                const Checked checked = check_records(records, canvas, tag);
                check(checked.records > 20, tag + "synth text did not go through the fitting canvas");
                elidedTotal += checked.elided;
                if (page != 0 && page != 4 && page != 5)  // grid pages
                    check_grid_geometry(controller.synth_panel().layout(), records, canvas, tag);
                (void)controller.synth_panel().scroll_grid(1);
            }
        }
        check(elidedTotal > 0, std::to_string(w) + "x" + std::to_string(h) + ": expected some elided synth values");
    }
}

void test_synth_tooltip() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 719);
    (void)controller.dispatch_action("window.toggle_synth");
    controller.synth_panel().set_page(SynthPanelPage::Modulation);
    // Row 21 "Amount" shows "<amount> | live <value>", which never fits the value slot.
    std::vector<FittedTextRecord> records;
    CellFitCanvas::set_record_sink(&records);
    MeasuringCanvas first;
    render_native_editor(first, controller, 1280, 719);
    CellFitCanvas::set_record_sink(nullptr);
    const auto live = std::find_if(records.begin(), records.end(), [](const FittedTextRecord& r) {
        return r.full.find("| live") != std::string::npos;
    });
    check(live != records.end() && live->elided, "Mod Matrix 'amount | live' value is elided");
    check(!first.drew(live != records.end() ? live->full : "?"), "full live value is not drawn without hover");
    if (live == records.end()) return;
    controller.pointer_move(live->cell.x + 3, live->cell.y + 3);
    MeasuringCanvas hovered;
    render_native_editor(hovered, controller, 1280, 719);
    check(hovered.drew(live->full), "hovering the elided value shows the full value in a tooltip");
    // Waveform names such as "Triangle" in the narrow wave slot of an LFO row.
    controller.pointer_move(5, 5);
    MeasuringCanvas away;
    render_native_editor(away, controller, 1280, 719);
    check(!away.drew(live->full), "tooltip disappears when the pointer leaves");
}

void test_chiptune() {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 719}, {640, 480}}) {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.resize(w, h);
        (void)controller.dispatch_action("window.toggle_chiptune");
        check(controller.chiptune_panel().open(), "chiptune opened");
        for (const std::string key : {"f1", "f2", "f3"}) {
            controller.key_down(key, false, false, false);
            const std::string tag = std::to_string(w) + "x" + std::to_string(h) + " chiptune " + key + ": ";
            std::vector<FittedTextRecord> records;
            CellFitCanvas::set_record_sink(&records);
            MeasuringCanvas canvas;
            render_native_editor(canvas, controller, w, h);
            CellFitCanvas::set_record_sink(nullptr);
            check(check_records(records, canvas, tag).records > 10, tag + "chiptune text did not go through the fitting canvas");
        }
    }
}

void test_settings() {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 719}, {640, 480}}) {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.resize(w, h);
        controller.open_settings(SettingScope::User, "General");
        const std::string tag = std::to_string(w) + "x" + std::to_string(h) + " settings: ";
        const auto rows = controller.settings_rows();
        std::size_t zoomRow = rows.size();
        for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i]->id == kUiZoomSettingId) zoomRow = i;
        check(zoomRow < rows.size(), tag + "UI zoom row missing");
        controller.settings_panel().selectedRow = zoomRow;
        controller.pointer_move(1, 1);  // hover nothing
        std::vector<FittedTextRecord> records;
        CellFitCanvas::set_record_sink(&records);
        MeasuringCanvas canvas;
        render_native_editor(canvas, controller, w, h);
        CellFitCanvas::set_record_sink(nullptr);
        const Checked checked = check_records(records, canvas, tag);
        check(checked.records > 10, tag + "settings text did not go through the fitting canvas");
        const auto zoom = std::find_if(records.begin(), records.end(), [](const FittedTextRecord& r) {
            return r.full.find("Ctrl+= / Ctrl+-") != std::string::npos;
        });
        check(zoom != records.end(), tag + "UI zoom value not drawn");
        if (zoom != records.end() && zoom->elided)
            check(canvas.drew(zoom->full), tag + "focused (selected) elided value shows its tooltip");
        if (w == 640) check(zoom != records.end() && zoom->elided, tag + "long UI zoom value is elided at 640 px");
    }
}

} // namespace

int main() {
    test_elide_helper();
    test_synth_pages();
    test_synth_tooltip();
    test_chiptune();
    test_settings();
    if (g_failures == 0) std::printf("editor text fit tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
