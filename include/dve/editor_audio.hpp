#pragma once

#include <array>

#include "dve/audio/mixer.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

struct AudioPanelLayout {
    UiRect panel{};
    UiRect titleBar{};
    UiRect closeButton{};
    UiRect panicButton{};
    UiRect testEventButton{};
    std::array<UiRect, audio::kAudioBusCount> busRows{};
    std::array<UiRect, audio::kAudioBusCount> muteButtons{};
    std::array<UiRect, audio::kAudioBusCount> gainDownButtons{};
    std::array<UiRect, audio::kAudioBusCount> gainUpButtons{};
};

class EditorAudioPanel {
public:
    [[nodiscard]] bool open() const noexcept { return open_; }
    void set_open(bool value) noexcept { open_ = value; }
    void toggle() noexcept { open_ = !open_; }
    [[nodiscard]] const AudioPanelLayout& layout() const noexcept { return layout_; }
    void resize(int width, int height, float uiScale = 1.0F) noexcept;
    bool pointer_down(int x, int y, audio::AudioMixer& mixer) noexcept;

private:
    AudioPanelLayout layout_{};
    bool open_{};
};

} // namespace dve::editor
