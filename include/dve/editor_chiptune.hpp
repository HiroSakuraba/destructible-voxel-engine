#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "dve/audio/chiptune_authoring.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

enum class ChiptunePanelPage : std::uint8_t { Pattern, Instrument, Sfx };
inline constexpr std::size_t kChiptuneVisibleRows = 16U;
inline constexpr std::size_t kChiptuneVisibleOrders = 12U;

struct ChiptunePanelLayout {
    UiRect panel{};
    UiRect titleBar{};
    UiRect closeButton{};
    std::array<UiRect, 3> tabs{};
    UiRect playSongButton{};
    UiRect playInstrumentButton{};
    UiRect stopButton{};
    UiRect saveButton{};
    UiRect openButton{};
    UiRect undoButton{};
    UiRect redoButton{};
    UiRect copyButton{};
    UiRect cutButton{};
    UiRect pasteButton{};
    UiRect songBusButton{};
    UiRect instrumentBusButton{};

    UiRect orderList{};
    std::array<UiRect, kChiptuneVisibleOrders> orderRows{};
    UiRect addOrderButton{};
    UiRect deleteOrderButton{};
    UiRect duplicatePatternButton{};
    UiRect addPatternButton{};
    UiRect deletePatternButton{};
    UiRect patternGrid{};
    std::array<std::array<UiRect, audio::kChiptuneMaxChannels>, kChiptuneVisibleRows> cells{};
    UiRect effectPreviousButton{};
    UiRect effectNextButton{};
    UiRect effectParamDownButton{};
    UiRect effectParamUpButton{};

    UiRect instrumentPreviousButton{};
    UiRect instrumentNextButton{};
    UiRect addInstrumentButton{};
    UiRect deleteInstrumentButton{};
    UiRect wavePreviousButton{};
    UiRect waveNextButton{};
    UiRect envelopeCanvas{};
    UiRect wavetableCanvas{};
    UiRect normalizeWavetableButton{};
    UiRect removeDcButton{};
    std::array<UiRect, 5> wavetableShapeButtons{};

    std::array<UiRect, 8> sfxPresetButtons{};
    UiRect sfxBaseDownButton{};
    UiRect sfxBaseUpButton{};
    UiRect sfxDurationDownButton{};
    UiRect sfxDurationUpButton{};
    UiRect sfxGainDownButton{};
    UiRect sfxGainUpButton{};
    UiRect sfxPanDownButton{};
    UiRect sfxPanUpButton{};
    UiRect applySfxButton{};
    UiRect auditionSfxButton{};
    std::array<UiRect, 24> pianoKeys{};
};

class EditorChiptunePanel {
public:
    EditorChiptunePanel();

    [[nodiscard]] bool open() const noexcept { return open_; }
    void set_open(bool value, audio::AudioMixer& mixer) noexcept;
    void toggle(audio::AudioMixer& mixer) noexcept { set_open(!open_, mixer); }
    [[nodiscard]] const ChiptunePanelLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] ChiptunePanelPage page() const noexcept { return page_; }
    [[nodiscard]] audio::ChiptuneAuthoringSession& session() noexcept { return session_; }
    [[nodiscard]] const audio::ChiptuneAuthoringSession& session() const noexcept { return session_; }
    [[nodiscard]] std::string_view status() const noexcept { return status_; }
    [[nodiscard]] const std::filesystem::path& document_path() const noexcept { return documentPath_; }
    [[nodiscard]] std::uint32_t first_visible_row() const noexcept { return firstVisibleRow_; }
    [[nodiscard]] std::uint32_t first_visible_order() const noexcept { return firstVisibleOrder_; }

    void set_document_path(std::filesystem::path path) noexcept;
    void resize(int width, int height, float uiScale = 1.0F) noexcept;
    bool pointer_down(int x, int y, audio::AudioMixer& mixer) noexcept;
    bool pointer_move(int x, int y) noexcept;
    bool pointer_up(int x, int y) noexcept;
    bool key_down(std::string_view key, bool control, bool shift, bool alt,
                  audio::AudioMixer& mixer) noexcept;

private:
    void set_status(std::string value) noexcept { status_ = std::move(value); }
    void keep_cursor_visible() noexcept;
    void cycle_bus(bool songBus, int direction) noexcept;
    void cycle_effect(int direction) noexcept;
    void adjust_effect_parameter(int direction) noexcept;
    void cycle_instrument(int direction) noexcept;
    void cycle_wave(int direction) noexcept;
    void draw_envelope(int x, int y) noexcept;
    void draw_wavetable(int x, int y) noexcept;
    void play_song(audio::AudioMixer& mixer) noexcept;
    void play_instrument(audio::AudioMixer& mixer, int midiNote = -1) noexcept;
    void save() noexcept;
    void load() noexcept;

    ChiptunePanelLayout layout_{};
    audio::ChiptuneAuthoringSession session_{};
    bool open_{};
    ChiptunePanelPage page_{ChiptunePanelPage::Pattern};
    std::filesystem::path documentPath_{"assets/chiptune/editor_song.dvechip"};
    std::string status_{"Tracker ready"};
    std::uint32_t firstVisibleRow_{};
    std::uint32_t firstVisibleOrder_{};
    bool envelopeDrawing_{};
    bool wavetableDrawing_{};
};

[[nodiscard]] std::string chip_note_display(std::uint8_t note);
[[nodiscard]] std::string chip_cell_display(const audio::ChipCell& cell);

} // namespace dve::editor
