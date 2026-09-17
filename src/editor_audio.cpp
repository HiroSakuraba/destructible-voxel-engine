#include "dve/editor_audio.hpp"

#include <algorithm>

namespace dve::editor {

void EditorAudioPanel::resize(int width, int height, float uiScale) noexcept {
    const int panelWidth = std::clamp(static_cast<int>(520.0F * uiScale), 430, std::max(430, width - 30));
    const int panelHeight = std::clamp(static_cast<int>(390.0F * uiScale), 330, std::max(330, height - 60));
    const int x = std::max(15, (width - panelWidth) / 2);
    const int y = std::max(40, (height - panelHeight) / 2);
    layout_.panel = {x, y, panelWidth, panelHeight};
    layout_.titleBar = {x, y, panelWidth, 34};
    layout_.closeButton = {x + panelWidth - 31, y + 5, 24, 24};
    layout_.panicButton = {x + 12, y + 45, 126, 26};
    layout_.testEventButton = {x + 146, y + 45, 126, 26};
    const int rowY = y + 82;
    const int rowHeight = 34;
    for (std::size_t i = 0; i < audio::kAudioBusCount; ++i) {
        layout_.busRows[i] = {x + 12, rowY + static_cast<int>(i) * rowHeight, panelWidth - 24, rowHeight - 4};
        layout_.muteButtons[i] = {x + panelWidth - 190, layout_.busRows[i].y + 3, 46, 24};
        layout_.gainDownButtons[i] = {x + panelWidth - 134, layout_.busRows[i].y + 3, 28, 24};
        layout_.gainUpButtons[i] = {x + panelWidth - 38, layout_.busRows[i].y + 3, 28, 24};
    }
}

bool EditorAudioPanel::pointer_down(int x, int y, audio::AudioMixer& mixer) noexcept {
    if (!open_ || !layout_.panel.contains(x, y)) return false;
    if (layout_.closeButton.contains(x, y)) { open_ = false; return true; }
    if (layout_.panicButton.contains(x, y)) { mixer.all_sounds_off(); return true; }
    if (layout_.testEventButton.contains(x, y)) {
        const std::uint64_t start = mixer.current_frame();
        (void)mixer.synthesizer().note_on(48U, 0.65F, 0U, start);
        (void)mixer.synthesizer().note_on(55U, 0.55F, 0U, start + 120U);
        (void)mixer.synthesizer().note_on(60U, 0.45F, 0U, start + 240U);
        (void)mixer.synthesizer().note_off(48U, 0.0F, 0U, start + 12000U);
        (void)mixer.synthesizer().note_off(55U, 0.0F, 0U, start + 13000U);
        (void)mixer.synthesizer().note_off(60U, 0.0F, 0U, start + 14000U);
        return true;
    }
    for (std::size_t i = 0; i < audio::kAudioBusCount; ++i) {
        const auto bus = static_cast<audio::AudioBusId>(i);
        audio::AudioBusParameters parameters = mixer.bus_parameters(bus);
        if (layout_.muteButtons[i].contains(x, y)) {
            parameters.mute = !parameters.mute;
            (void)mixer.set_bus_parameters(bus, parameters);
            return true;
        }
        if (layout_.gainDownButtons[i].contains(x, y)) {
            parameters.gain = std::max(0.0F, parameters.gain - 0.05F);
            (void)mixer.set_bus_parameters(bus, parameters);
            return true;
        }
        if (layout_.gainUpButtons[i].contains(x, y)) {
            parameters.gain = std::min(2.0F, parameters.gain + 0.05F);
            (void)mixer.set_bus_parameters(bus, parameters);
            return true;
        }
    }
    return true;
}

} // namespace dve::editor
