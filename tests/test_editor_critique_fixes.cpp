// Fix verification tests for the Phase 4 critique editor item (#5):
// all 18 advanced oscillator rows must be laid out inside the panel rect
// (3 columns x 6 rows) so they are visible and clickable.
#include <iostream>
#include <stdexcept>

#include "dve/editor_native.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

bool inside_rect(const dve::editor::UiRect& inner, const dve::editor::UiRect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

void check_advanced_rows(dve::editor::NativeEditorController& controller, int width, int height) {
    using namespace dve::editor;
    controller.synth_panel().resize(width, height);
    require(controller.synth_panel().page() == SynthPanelPage::Oscillators,
            "synth panel was not on the Oscillators page");
    // Short windows scroll the Oscillators page: check each advanced row at a scroll position
    // where it is visible (rows outside the viewport have empty rects).
    (void)controller.synth_panel().scroll_grid(-100);
    while (controller.synth_panel().layout().oscillatorAdvancedRows.back().width == 0 &&
           controller.synth_panel().scroll_grid(1)) {}
    const auto layout = controller.synth_panel().layout();
    const UiRect panel = layout.panel;
    require(panel.width > 0 && panel.height > 0, "synth panel had no size");

    std::size_t distinctColumns = 0;
    int columnX[3] = {0, 0, 0};
    for (std::size_t i = 0; i < layout.oscillatorAdvancedRows.size(); ++i) {
        const UiRect row = layout.oscillatorAdvancedRows[i];
        require(row.width > 0 && row.height > 0, "advanced oscillator row had no size");
        require(inside_rect(row, panel), "advanced oscillator row is outside the panel");
        const UiRect down = layout.oscillatorAdvancedDownButtons[i];
        const UiRect up = layout.oscillatorAdvancedUpButtons[i];
        require(inside_rect(down, row), "advanced row down button is outside its row");
        require(inside_rect(up, row), "advanced row up button is outside its row");
        bool known = false;
        for (std::size_t c = 0; c < distinctColumns; ++c) known = known || columnX[c] == row.x;
        if (!known) {
            require(distinctColumns < 3, "more than 3 columns of advanced rows");
            columnX[distinctColumns++] = row.x;
        }
    }
    require(layout.oscillatorAdvancedRows.size() == kSynthOscillatorAdvancedPropertyCount,
            "advanced row layout count mismatch");
    require(distinctColumns == 3, "expected 3 columns of advanced rows");

    // Grid integrity: rows must not overlap each other.
    for (std::size_t i = 0; i < layout.oscillatorAdvancedRows.size(); ++i) {
        for (std::size_t j = i + 1; j < layout.oscillatorAdvancedRows.size(); ++j) {
            const UiRect a = layout.oscillatorAdvancedRows[i];
            const UiRect b = layout.oscillatorAdvancedRows[j];
            const bool overlap = a.x < b.x + b.width && b.x < a.x + a.width &&
                                 a.y < b.y + b.height && b.y < a.y + a.height;
            require(!overlap, "advanced oscillator rows overlap");
        }
    }

    // Every row is clickable: the panel must claim a press on each up button.
    for (std::size_t i = 0; i < layout.oscillatorAdvancedUpButtons.size(); ++i) {
        const UiRect up = layout.oscillatorAdvancedUpButtons[i];
        const int x = up.x + up.width / 2;
        const int y = up.y + up.height / 2;
        require(controller.synth_panel().pointer_down(x, y, controller.synthesizer()),
                "advanced row up button click was not claimed");
        controller.synth_panel().pointer_up(x, y, controller.synthesizer());
    }
}
} // namespace

int main() {
    try {
        using namespace dve::editor;
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        require(controller.dispatch_action("window.toggle_synth"), "synth menu action failed");
        require(controller.synth_panel().open(), "synth panel did not open");

        check_advanced_rows(controller, 1600, 1000);
        check_advanced_rows(controller, 1000, 700);
        std::cout << "dve_editor_critique_fixes_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_critique_fixes_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
