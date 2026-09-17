#include <iostream>
#include <stdexcept>
#include <vector>

#include "dve/editor_native.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    using namespace dve::editor;
    try {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        require(controller.workspace().menus().find("window.toggle_audio") != nullptr,
                "Audio Mixer is missing from the Window menu");
        require(controller.dispatch_action("window.toggle_audio"), "audio panel menu action failed");
        require(controller.audio_panel().open(), "audio panel did not open");
        controller.resize(1280, 800);
        const auto layout = controller.audio_panel().layout();
        require(layout.panel.width > 400 && layout.busRows.size() == dve::audio::kAudioBusCount,
                "audio panel layout is incomplete");

        const auto before = controller.audio_mixer().bus_parameters(dve::audio::AudioBusId::Effects);
        controller.pointer_down(PointerButton::Primary,
                                layout.gainDownButtons[dve::audio::audio_bus_index(dve::audio::AudioBusId::Effects)].x + 2,
                                layout.gainDownButtons[dve::audio::audio_bus_index(dve::audio::AudioBusId::Effects)].y + 2);
        std::vector<float> audio(512U * 2U);
        controller.audio_mixer().render(audio);
        const auto after = controller.audio_mixer().bus_parameters(dve::audio::AudioBusId::Effects);
        require(after.gain < before.gain, "audio bus gain control did not change the mixer");

        controller.pointer_down(PointerButton::Primary, layout.testEventButton.x + 2, layout.testEventButton.y + 2);
        controller.audio_mixer().render(audio);
        require(controller.audio_mixer().meters().synthVoices >= 1U, "audio panel test event did not reach the mixer");

        require(controller.dispatch_action("window.toggle_audio"), "audio close action failed");
        require(!controller.audio_panel().open(), "audio panel did not close");
        controller.key_down("5", true, false, false);
        require(controller.audio_panel().open(), "Ctrl+5 did not open audio panel");

        std::cout << "dve_editor_audio_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_audio_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
