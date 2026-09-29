#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "dve/audio/patch_search_browser.hpp"
#include "dve/audio/synthesizer.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

enum class SynthPanelPage : std::uint8_t {
    Oscillators,
    FilterEnvelope,
    Modulation,
    Performance,
    Effects,
    Presets,
    Expression,
    Generative,
};
inline constexpr std::size_t kSynthPanelPageCount = 8;
inline constexpr std::size_t kSynthParameterRowCount = 30;
inline constexpr std::size_t kSynthOscillatorAdvancedPropertyCount = 18;
inline constexpr std::size_t kSynthSpectralAdvancedRowCount = 14;  // Phase 5: spectral section rows
inline constexpr std::size_t kSynthArpStepPropertyCount = 18;
inline constexpr std::size_t kSynthPresetVisibleEntryCount = 8;

struct SynthPanelLayout {
    UiRect panel{};
    UiRect titleBar{};
    UiRect closeButton{};
    UiRect panicButton{};
    UiRect resetButton{};
    UiRect octaveDownButton{};
    UiRect octaveUpButton{};
    UiRect midiThruButton{};
    UiRect searchButton{};  // Phase 6: toggles the patch-search browser panel
    std::array<UiRect, kSynthPanelPageCount> tabButtons{};

    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorRows{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorEnableButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorWaveButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorGainDownButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorGainUpButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorTuneDownButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorTuneUpButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorFineDownButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorFineUpButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorPwmDownButtons{};
    std::array<UiRect, audio::kSynthOscillatorCount> oscillatorPwmUpButtons{};
    std::array<UiRect, kSynthOscillatorAdvancedPropertyCount> oscillatorAdvancedRows{};
    std::array<UiRect, kSynthOscillatorAdvancedPropertyCount> oscillatorAdvancedDownButtons{};
    std::array<UiRect, kSynthOscillatorAdvancedPropertyCount> oscillatorAdvancedUpButtons{};
    UiRect wavetableCanvas{};
    std::array<UiRect, audio::kWavetableFrameCount> wavetableFrameButtons{};
    UiRect wavetableNormalizeButton{};
    UiRect wavetableRemoveDcButton{};
    UiRect wavetableAlignButton{};

    std::array<UiRect, kSynthParameterRowCount> parameterRows{};
    std::array<UiRect, kSynthParameterRowCount> parameterDownButtons{};
    std::array<UiRect, kSynthParameterRowCount> parameterUpButtons{};
    std::array<UiRect, kSynthParameterRowCount> parameterToggleButtons{};

    std::array<UiRect, audio::kSynthMacroCount> macroRows{};
    std::array<UiRect, audio::kSynthMacroCount> macroDownButtons{};
    std::array<UiRect, audio::kSynthMacroCount> macroUpButtons{};

    std::array<UiRect, audio::kArpeggiatorStepCount> arpeggiatorStepButtons{};
    std::array<UiRect, kSynthArpStepPropertyCount> arpeggiatorStepRows{};
    std::array<UiRect, kSynthArpStepPropertyCount> arpeggiatorStepDownButtons{};
    std::array<UiRect, kSynthArpStepPropertyCount> arpeggiatorStepUpButtons{};
    std::array<UiRect, kSynthArpStepPropertyCount> arpeggiatorStepToggleButtons{};

    std::array<UiRect, 13> effectRows{};
    std::array<UiRect, 13> effectToggleButtons{};
    static constexpr std::size_t kSynthEffectParamCount = 6;
    std::array<UiRect, kSynthEffectParamCount> effectParamRows{};
    std::array<UiRect, kSynthEffectParamCount> effectParamDownButtons{};
    std::array<UiRect, kSynthEffectParamCount> effectParamUpButtons{};
    std::array<UiRect, kSynthEffectParamCount> effectParamToggleButtons{};

    UiRect presetScanButton{};
    UiRect presetPreviousButton{};
    UiRect presetNextButton{};
    UiRect presetLoadButton{};
    UiRect presetCaptureAButton{};
    UiRect presetCaptureBButton{};
    UiRect presetMorphDownButton{};
    UiRect presetMorphUpButton{};
    std::array<UiRect, kSynthPresetVisibleEntryCount> presetEntryButtons{};

    std::array<UiRect, 24> pianoKeys{};
};

class EditorSynthPanel {
public:
    [[nodiscard]] bool open() const noexcept { return open_; }
    void set_open(bool open, audio::Synthesizer& synth) noexcept;
    void toggle(audio::Synthesizer& synth) noexcept { set_open(!open_, synth); }
    [[nodiscard]] const SynthPanelLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] int octave() const noexcept { return octave_; }
    [[nodiscard]] std::size_t selected_oscillator() const noexcept { return selectedOscillator_; }
    [[nodiscard]] std::size_t selected_arpeggiator_step() const noexcept { return selectedArpeggiatorStep_; }
    [[nodiscard]] std::size_t selected_effect() const noexcept { return selectedEffect_; }
    [[nodiscard]] std::size_t selected_modulation_slot() const noexcept { return selectedModulationSlot_; }
    [[nodiscard]] std::size_t selected_preset_entry() const noexcept { return selectedPresetEntry_; }
    [[nodiscard]] std::size_t selected_sequencer_lane() const noexcept { return selectedSequencerLane_; }
    [[nodiscard]] std::size_t selected_midi_learn_mapping() const noexcept { return selectedMidiLearnMapping_; }
    [[nodiscard]] std::size_t selected_wavetable_frame() const noexcept { return selectedWavetableFrame_; }
    [[nodiscard]] float preset_morph_amount() const noexcept { return presetMorphAmount_; }
    [[nodiscard]] std::string_view preset_status() const noexcept { return presetStatus_; }
    [[nodiscard]] const audio::SynthPresetLibrary& preset_library() const noexcept { return presetLibrary_; }
    [[nodiscard]] PatchSearchBrowserPanel& search_panel() noexcept { return searchPanel_; }
    [[nodiscard]] const PatchSearchBrowserPanel& search_panel() const noexcept { return searchPanel_; }
    [[nodiscard]] SynthPanelPage page() const noexcept { return page_; }
    // Wavetable drawing is coalesced: strokes edit a local draft and publish it
    // to the synth at most once per kWavetableDrawPublishInterval (plus always
    // on pointer-up / flush), instead of a set_preset (and cook) per mouse move.
    static constexpr std::chrono::milliseconds kWavetableDrawPublishInterval{33};
    [[nodiscard]] std::uint64_t wavetable_draw_publish_count() const noexcept { return wavetableDrawPublishes_; }
    [[nodiscard]] bool wavetable_draw_pending() const noexcept { return wavetableDraftDirty_; }
    // Publishes a pending wavetable draft now.
    void flush_wavetable_draft(audio::Synthesizer& synth) noexcept;
    // Per-UI-update trailing edge: publishes a pending draft once the throttle
    // interval has elapsed (so a paused-but-held stroke still becomes audible).
    void flush_wavetable_draft_if_due(audio::Synthesizer& synth) noexcept;

    void set_preset_directory(std::filesystem::path directory) noexcept;
    bool refresh_preset_library() noexcept;

    void resize(int width, int height, float uiScale = 1.0F) noexcept;
    bool pointer_down(int x, int y, audio::Synthesizer& synth) noexcept;
    bool pointer_move(int x, int y, audio::Synthesizer& synth) noexcept;
    bool pointer_up(int x, int y, audio::Synthesizer& synth) noexcept;
    bool key_down(std::string_view key, audio::Synthesizer& synth) noexcept;
    bool key_up(std::string_view key, audio::Synthesizer& synth) noexcept;

private:
    static int keyboard_note(std::string_view key, int octave) noexcept;
    void release_panel_notes(audio::Synthesizer& synth) noexcept;
    void cycle_waveform(std::size_t oscillator, int direction, audio::Synthesizer& synth) noexcept;
    void toggle_effect(std::size_t index, audio::Synthesizer& synth) noexcept;
    void adjust_effect_param(std::size_t paramIndex, int direction, audio::Synthesizer& synth) noexcept;
    void toggle_effect_param(std::size_t paramIndex, audio::Synthesizer& synth) noexcept;
    void adjust_parameter(std::size_t index, int direction, audio::Synthesizer& synth) noexcept;
    void toggle_parameter(std::size_t index, audio::Synthesizer& synth) noexcept;
    void adjust_oscillator_advanced(std::size_t index, int direction, audio::Synthesizer& synth) noexcept;
    void adjust_macro(std::size_t index, int direction, audio::Synthesizer& synth) noexcept;
    void adjust_arpeggiator_step_parameter(std::size_t index, int direction,
                                           audio::Synthesizer& synth) noexcept;
    void toggle_arpeggiator_step_parameter(std::size_t index, audio::Synthesizer& synth) noexcept;
    void load_selected_preset(audio::Synthesizer& synth) noexcept;
    void apply_preset_morph(audio::Synthesizer& synth) noexcept;
    void draw_wavetable_point(int x, int y, audio::Synthesizer& synth) noexcept;

    SynthPanelLayout layout_{};
    bool open_{};
    int octave_{4};
    std::size_t selectedOscillator_{};
    std::size_t selectedArpeggiatorStep_{};
    std::size_t selectedEffect_{};
    std::size_t selectedModulationSlot_{};
    std::size_t selectedPresetEntry_{};
    std::size_t selectedSequencerLane_{};
    std::size_t selectedMidiLearnMapping_{};
    std::size_t selectedWavetableFrame_{};
    bool wavetableDrawing_{};
    int wavetableLastSample_{-1};
    float wavetableLastValue_{};
    std::optional<audio::SynthPreset> wavetableDraft_{};
    bool wavetableDraftDirty_{};
    std::chrono::steady_clock::time_point wavetableLastPublish_{};
    std::uint64_t wavetableDrawPublishes_{};
    SynthPanelPage page_{SynthPanelPage::Oscillators};
    int pointerNote_{-1};
    std::array<bool, 128> keyboardNotes_{};

    std::filesystem::path presetDirectory_{"assets/audio/presets"};
    audio::SynthPresetLibrary presetLibrary_{};
    std::string presetStatus_{"Preset library not scanned"};
    std::optional<audio::SynthPreset> compareA_{};
    std::optional<audio::SynthPreset> compareB_{};
    float presetMorphAmount_{0.5F};
    PatchSearchBrowserPanel searchPanel_;  // Phase 6: candidate browser overlay
    int lastWidth_{1280};
    int lastHeight_{800};
    float lastUiScale_{1.0F};
};

} // namespace dve::editor
