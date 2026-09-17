#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

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
}

int main() {
    using namespace dve;
    using namespace dve::editor;
    try {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        require(controller.workspace().menus().find("window.toggle_chiptune") != nullptr,
                "Chiptune Tracker is missing from the Window menu");
        require(controller.workspace().menus().find("audio.tracker") != nullptr,
                "Chiptune Tracker is missing from the Audio menu");
        require(controller.dispatch_action("window.toggle_chiptune"), "tracker action failed");
        require(controller.chiptune_panel().open(), "tracker panel did not open");
        controller.resize(1440, 900);
        auto layout = controller.chiptune_panel().layout();
        require(layout.panel.width >= 720 && layout.cells.size() == kChiptuneVisibleRows,
                "tracker layout is incomplete");

        const auto initial = controller.chiptune_panel().session().song().content_hash();
        const auto cellRect = layout.cells[0][0];
        controller.pointer_down(PointerButton::Primary, cellRect.x + 4, cellRect.y + 4);
        controller.key_down("x", false, false, false);
        const auto& session = controller.chiptune_panel().session();
        const auto pattern = session.song().order[0];
        require(dve::audio::chip_note_to_midi(session.song().patterns[pattern].at(0,0,session.song().channelCount).note) == 62,
                "native tracker keyboard did not enter a distinct note");
        require(session.song().content_hash() != initial, "native note edit did not change content");

        controller.key_down("escape", false, false, false);
        controller.key_down("down", false, true, false);
        controller.key_down("right", false, true, false);
        require(session.selection().has_value() && session.selection()->last_row() >= 1U &&
                    session.selection()->last_channel() >= 1U,
                "Shift+arrow did not create a rectangular tracker selection");
        controller.key_down("c", true, false, false);
        require(!session.clipboard().empty() && session.clipboard().rows >= 2U && session.clipboard().channels >= 2U,
                "Ctrl+C did not fill the rectangular tracker clipboard");
        controller.key_down("a", true, false, false);
        controller.key_down("z", true, false, false);
        require(session.can_redo(), "Ctrl+Z did not create redo state");
        controller.key_down("y", true, false, false);

        controller.pointer_down(PointerButton::Primary, layout.tabs[1].x + 3, layout.tabs[1].y + 3);
        require(controller.chiptune_panel().page() == ChiptunePanelPage::Instrument,
                "instrument tab did not activate");
        layout = controller.chiptune_panel().layout();
        controller.pointer_down(PointerButton::Primary,
                                layout.envelopeCanvas.x + layout.envelopeCanvas.width / 3,
                                layout.envelopeCanvas.y + layout.envelopeCanvas.height / 4);
        controller.pointer_up(PointerButton::Primary,
                              layout.envelopeCanvas.x + layout.envelopeCanvas.width / 3,
                              layout.envelopeCanvas.y + layout.envelopeCanvas.height / 4);
        require(!session.song().instruments[session.selected_instrument()].volume.values.empty(),
                "envelope drawing did not modify the instrument");

        const auto busBefore = session.song_bus();
        controller.pointer_down(PointerButton::Primary, layout.songBusButton.x + 3, layout.songBusButton.y + 3);
        require(session.song_bus() != busBefore, "song bus routing button did not cycle the bus");

        controller.pointer_down(PointerButton::Primary,
                                layout.playInstrumentButton.x + 3,
                                layout.playInstrumentButton.y + 3);
        std::vector<float> output(2048U * 2U);
        controller.audio_mixer().render(output);
        require(std::any_of(output.begin(), output.end(), [](float value){ return std::abs(value) > 1.0e-5F; }),
                "instrument preview produced silence in the native mixer");

        controller.pointer_down(PointerButton::Primary, layout.tabs[2].x + 3, layout.tabs[2].y + 3);
        require(controller.chiptune_panel().page() == ChiptunePanelPage::Sfx,
                "SFX tab did not activate");
        layout = controller.chiptune_panel().layout();
        controller.pointer_down(PointerButton::Primary,
                                layout.sfxPresetButtons[static_cast<std::size_t>(dve::audio::ChipSfxPreset::Explosion)].x + 3,
                                layout.sfxPresetButtons[static_cast<std::size_t>(dve::audio::ChipSfxPreset::Explosion)].y + 3);
        require(session.sfx_request().preset == dve::audio::ChipSfxPreset::Explosion,
                "SFX preset selection did not update");
        const int expectedPianoMidi = (session.octave() + 1) * 12 + 5;
        controller.pointer_down(PointerButton::Primary, layout.pianoKeys[5].x + 3, layout.pianoKeys[5].y + 3);
        require(session.sfx_request().baseMidi == expectedPianoMidi,
                "on-screen piano did not enter the SFX base note");
        controller.pointer_down(PointerButton::Primary, layout.applySfxButton.x + 3, layout.applySfxButton.y + 3);
        require(session.song().name == "explosion", "SFX apply did not replace the tracker document");

        CountingCanvas canvas;
        render_native_editor(canvas, controller, 1440, 900);
        require(canvas.fills > 20 && canvas.texts > 20, "native tracker did not render through the shared canvas");

        require(controller.dispatch_action("window.toggle_chiptune"), "tracker close action failed");
        require(!controller.chiptune_panel().open(), "tracker panel did not close");
        controller.key_down("9", true, false, false);
        require(controller.chiptune_panel().open(), "Ctrl+9 did not reopen Chiptune Tracker");

        std::cout << "dve_editor_chiptune_v217_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_chiptune_v217_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
