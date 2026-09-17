#include "dve/editor_synth.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

namespace dve::editor {
namespace {
bool contains(UiRect rect, int x, int y) noexcept { return rect.contains(x, y); }
std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
template <class Enum>
Enum cycle_enum(Enum value, unsigned count, int direction) noexcept {
    int index = static_cast<int>(value) + direction;
    while (index < 0) index += static_cast<int>(count);
    return static_cast<Enum>(index % static_cast<int>(count));
}
float stepped(float value, float delta, float minimum, float maximum, int direction) noexcept {
    return std::clamp(value + delta * static_cast<float>(direction), minimum, maximum);
}
std::int8_t cycle_oscillator_source(std::int8_t source, int direction) noexcept {
    int index = static_cast<int>(source) + 1 + direction;
    while (index < 0) index += static_cast<int>(audio::kSynthOscillatorCount + 1U);
    index %= static_cast<int>(audio::kSynthOscillatorCount + 1U);
    return static_cast<std::int8_t>(index - 1);
}
audio::FilterOversampling cycle_oversampling(audio::FilterOversampling value, int direction) noexcept {
    static constexpr std::array values{
        audio::FilterOversampling::X1, audio::FilterOversampling::X2, audio::FilterOversampling::X4};
    std::size_t index = 0;
    for (std::size_t i = 0; i < values.size(); ++i) if (values[i] == value) index = i;
    int next = static_cast<int>(index) + direction;
    while (next < 0) next += static_cast<int>(values.size());
    return values[static_cast<std::size_t>(next) % values.size()];
}
} // namespace

void EditorSynthPanel::set_open(bool openValue, audio::Synthesizer& synth) noexcept {
    if (open_ && !openValue) release_panel_notes(synth);
    open_ = openValue;
}

void EditorSynthPanel::set_preset_directory(std::filesystem::path directory) noexcept {
    try {
        presetDirectory_ = std::move(directory);
        presetStatus_ = "Preset library not scanned";
        selectedPresetEntry_ = 0U;
    } catch (...) {
        presetStatus_ = "Could not set preset directory";
    }
}

bool EditorSynthPanel::refresh_preset_library() noexcept {
    try {
        std::string error;
        if (!presetLibrary_.scan(presetDirectory_, &error)) {
            presetStatus_ = error.empty() ? "Preset scan failed" : error;
            return false;
        }
        if (selectedPresetEntry_ >= presetLibrary_.entries().size()) selectedPresetEntry_ = 0U;
        presetStatus_ = std::to_string(presetLibrary_.entries().size()) + " presets indexed";
        return true;
    } catch (...) {
        presetStatus_ = "Preset scan failed";
        return false;
    }
}

void EditorSynthPanel::resize(int width, int height, float uiScale) noexcept {
    const int panelWidth = std::min(width - 24, std::max(940, static_cast<int>(1240.0F * uiScale)));
    const int panelHeight = std::min(height - 44, std::max(640, static_cast<int>(790.0F * uiScale)));
    layout_.panel = {(width - panelWidth) / 2, std::max(22, (height - panelHeight) / 2), panelWidth, panelHeight};
    layout_.titleBar = {layout_.panel.x, layout_.panel.y, panelWidth, 34};
    layout_.closeButton = {layout_.panel.x + panelWidth - 32, layout_.panel.y + 5, 24, 24};
    layout_.panicButton = {layout_.panel.x + panelWidth - 154, layout_.panel.y + 5, 108, 24};
    layout_.resetButton = {layout_.panel.x + 12, layout_.panel.y + 43, 92, 24};
    layout_.octaveDownButton = {layout_.panel.x + 116, layout_.panel.y + 43, 30, 24};
    layout_.octaveUpButton = {layout_.panel.x + 184, layout_.panel.y + 43, 30, 24};
    layout_.midiThruButton = {layout_.panel.x + 228, layout_.panel.y + 43, 106, 24};

    const int tabsLeft = layout_.panel.x + 344;
    const int tabWidth = std::max(80, (panelWidth - 356) / static_cast<int>(kSynthPanelPageCount));
    for (std::size_t i = 0; i < layout_.tabButtons.size(); ++i)
        layout_.tabButtons[i] = {tabsLeft + static_cast<int>(i) * tabWidth,
                                 layout_.panel.y + 43, tabWidth - 4, 24};

    const int left = layout_.panel.x + 12;
    const int top = layout_.panel.y + 82;
    const int contentWidth = panelWidth - 24;
    constexpr int oscillatorRowHeight = 34;
    for (std::size_t i = 0; i < layout_.oscillatorRows.size(); ++i) {
        const int y = top + static_cast<int>(i) * oscillatorRowHeight;
        layout_.oscillatorRows[i] = {left, y, contentWidth, oscillatorRowHeight - 3};
        layout_.oscillatorEnableButtons[i] = {left + 4, y + 4, 30, 23};
        layout_.oscillatorWaveButtons[i] = {left + 88, y + 4, 112, 23};
        layout_.oscillatorGainDownButtons[i] = {left + 254, y + 4, 24, 23};
        layout_.oscillatorGainUpButtons[i] = {left + 342, y + 4, 24, 23};
        layout_.oscillatorTuneDownButtons[i] = {left + 430, y + 4, 24, 23};
        layout_.oscillatorTuneUpButtons[i] = {left + 506, y + 4, 24, 23};
        layout_.oscillatorFineDownButtons[i] = {left + 600, y + 4, 24, 23};
        layout_.oscillatorFineUpButtons[i] = {left + 688, y + 4, 24, 23};
        layout_.oscillatorPwmDownButtons[i] = {left + 790, y + 4, 24, 23};
        layout_.oscillatorPwmUpButtons[i] = {left + 886, y + 4, 24, 23};
    }

    constexpr int parameterRowHeight = 29;
    for (std::size_t i = 0; i < layout_.parameterRows.size(); ++i) {
        const int column = static_cast<int>(i / 8U);
        const int row = static_cast<int>(i % 8U);
        const int columnWidth = (contentWidth - 20) / 3;
        const int x = left + column * (columnWidth + 10);
        const int y = top + row * parameterRowHeight;
        layout_.parameterRows[i] = {x, y, columnWidth, parameterRowHeight - 3};
        layout_.parameterDownButtons[i] = {x + columnWidth - 116, y + 2, 27, 22};
        layout_.parameterUpButtons[i] = {x + columnWidth - 31, y + 2, 27, 22};
        layout_.parameterToggleButtons[i] = {x + columnWidth - 82, y + 2, 78, 22};
    }

    const int oscillatorAdvancedTop = top + static_cast<int>(audio::kSynthOscillatorCount) * oscillatorRowHeight + 7;
    for (std::size_t i = 0; i < layout_.oscillatorAdvancedRows.size(); ++i) {
        const int column = static_cast<int>(i / 5U);
        const int row = static_cast<int>(i % 5U);
        const int columnWidth = (contentWidth - 10) / 2;
        const int x = left + column * (columnWidth + 10);
        const int y = oscillatorAdvancedTop + row * parameterRowHeight;
        layout_.oscillatorAdvancedRows[i] = {x, y, columnWidth, parameterRowHeight - 3};
        layout_.oscillatorAdvancedDownButtons[i] = {x + columnWidth - 116, y + 2, 27, 22};
        layout_.oscillatorAdvancedUpButtons[i] = {x + columnWidth - 31, y + 2, 27, 22};
    }
    layout_.wavetableCanvas = {left, oscillatorAdvancedTop + 5 * parameterRowHeight + 6, contentWidth, 76};
    const int wtButtonWidth = std::max(44, (contentWidth - 330) / static_cast<int>(audio::kWavetableFrameCount));
    for (std::size_t i = 0; i < layout_.wavetableFrameButtons.size(); ++i)
        layout_.wavetableFrameButtons[i] = {left + static_cast<int>(i) * wtButtonWidth,
                                            layout_.wavetableCanvas.y + layout_.wavetableCanvas.height + 5,
                                            wtButtonWidth - 3, 23};
    const int toolsX = left + static_cast<int>(layout_.wavetableFrameButtons.size()) * wtButtonWidth + 8;
    layout_.wavetableNormalizeButton = {toolsX, layout_.wavetableCanvas.y + layout_.wavetableCanvas.height + 5, 96, 23};
    layout_.wavetableRemoveDcButton = {toolsX + 102, layout_.wavetableCanvas.y + layout_.wavetableCanvas.height + 5, 82, 23};
    layout_.wavetableAlignButton = {toolsX + 190, layout_.wavetableCanvas.y + layout_.wavetableCanvas.height + 5, 82, 23};

    const int macroTop = top + 8 * parameterRowHeight + 14;
    const int macroWidth = (contentWidth - 30) / 4;
    for (std::size_t i = 0; i < layout_.macroRows.size(); ++i) {
        const int x = left + static_cast<int>(i) * (macroWidth + 10);
        layout_.macroRows[i] = {x, macroTop, macroWidth, 28};
        layout_.macroDownButtons[i] = {x + macroWidth - 102, macroTop + 3, 26, 22};
        layout_.macroUpButtons[i] = {x + macroWidth - 30, macroTop + 3, 26, 22};
    }

    const int arpStepTop = top + 8 * parameterRowHeight + 16;
    const int arpStepWidth = std::max(34, contentWidth / static_cast<int>(audio::kArpeggiatorStepCount));
    for (std::size_t i = 0; i < layout_.arpeggiatorStepButtons.size(); ++i) {
        const int x = left + static_cast<int>(i) * arpStepWidth;
        const int right = i + 1U == layout_.arpeggiatorStepButtons.size() ? left + contentWidth : x + arpStepWidth;
        layout_.arpeggiatorStepButtons[i] = {x, arpStepTop, std::max(22, right - x - 3), 23};
    }
    for (std::size_t i = 0; i < layout_.arpeggiatorStepRows.size(); ++i) {
        const int column = static_cast<int>(i / 4U);
        const int row = static_cast<int>(i % 4U);
        const int columnWidth = (contentWidth - 30) / 4;
        const int x = left + column * (columnWidth + 10);
        const int y = arpStepTop + 32 + row * parameterRowHeight;
        layout_.arpeggiatorStepRows[i] = {x, y, columnWidth, parameterRowHeight - 3};
        layout_.arpeggiatorStepDownButtons[i] = {x + columnWidth - 116, y + 2, 27, 22};
        layout_.arpeggiatorStepUpButtons[i] = {x + columnWidth - 31, y + 2, 27, 22};
        layout_.arpeggiatorStepToggleButtons[i] = {x + columnWidth - 82, y + 2, 78, 22};
    }

    for (std::size_t i = 0; i < layout_.effectRows.size(); ++i) {
        const int column = static_cast<int>(i / 4U);
        const int row = static_cast<int>(i % 4U);
        const int columnWidth = (contentWidth - 10) / 2;
        const int x = left + column * (columnWidth + 10);
        const int y = top + row * 45;
        layout_.effectRows[i] = {x, y, columnWidth, 38};
        layout_.effectToggleButtons[i] = {x + columnWidth - 70, y + 7, 60, 24};
    }

    layout_.presetScanButton = {left, top, 118, 26};
    layout_.presetPreviousButton = {left + 128, top, 36, 26};
    layout_.presetNextButton = {left + 170, top, 36, 26};
    layout_.presetLoadButton = {left + 214, top, 90, 26};
    layout_.presetCaptureAButton = {left + 318, top, 90, 26};
    layout_.presetCaptureBButton = {left + 414, top, 90, 26};
    layout_.presetMorphDownButton = {left + 520, top, 36, 26};
    layout_.presetMorphUpButton = {left + 652, top, 36, 26};
    for (std::size_t i = 0; i < layout_.presetEntryButtons.size(); ++i) {
        const int column = static_cast<int>(i / 4U);
        const int row = static_cast<int>(i % 4U);
        const int columnWidth = (contentWidth - 10) / 2;
        const int x = left + column * (columnWidth + 10);
        const int y = top + 274 + row * 34;
        layout_.presetEntryButtons[i] = {x, y, columnWidth, 29};
    }

    const int pianoY = layout_.panel.y + panelHeight - 120;
    const int pianoWidth = panelWidth - 24;
    const int keyWidth = std::max(16, pianoWidth / 24);
    for (std::size_t i = 0; i < layout_.pianoKeys.size(); ++i) {
        const int x = left + static_cast<int>(i) * keyWidth;
        const int next = i + 1U == layout_.pianoKeys.size() ? left + pianoWidth : x + keyWidth;
        layout_.pianoKeys[i] = {x, pianoY, next - x, 90};
    }
}

void EditorSynthPanel::release_panel_notes(audio::Synthesizer& synth) noexcept {
    for (std::size_t note = 0; note < keyboardNotes_.size(); ++note) {
        if (keyboardNotes_[note]) (void)synth.note_off(static_cast<std::uint8_t>(note));
        keyboardNotes_[note] = false;
    }
    if (pointerNote_ >= 0) (void)synth.note_off(static_cast<std::uint8_t>(pointerNote_));
    pointerNote_ = -1;
}

void EditorSynthPanel::cycle_waveform(std::size_t oscillator, int direction,
                                      audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    preset.oscillators[oscillator].waveform = cycle_enum(
        preset.oscillators[oscillator].waveform, 13U, direction);
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_effect(std::size_t index, audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    switch (index) {
        case 0: preset.distortion.enabled = !preset.distortion.enabled; break;
        case 1: preset.eq.enabled = !preset.eq.enabled; break;
        case 2: preset.chorus.enabled = !preset.chorus.enabled; break;
        case 3: preset.phaser.enabled = !preset.phaser.enabled; break;
        case 4: preset.delay.enabled = !preset.delay.enabled; break;
        case 5: preset.reverb.enabled = !preset.reverb.enabled; break;
        case 6: preset.compressor.enabled = !preset.compressor.enabled; break;
        case 7: preset.limiter.enabled = !preset.limiter.enabled; break;
        default: return;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_oscillator_advanced(std::size_t index, int direction,
                                                   audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    auto& oscillator = preset.oscillators[selectedOscillator_];
    if (oscillator.waveform == audio::OscillatorWaveform::Sample) {
        switch (index) {
            case 0: oscillator.sampleStart = stepped(oscillator.sampleStart, 0.025F, 0.0F, oscillator.sampleEnd - 0.01F, direction); break;
            case 1: oscillator.sampleEnd = stepped(oscillator.sampleEnd, 0.025F, oscillator.sampleStart + 0.01F, 1.0F, direction); break;
            case 2: oscillator.sampleLoopStart = stepped(oscillator.sampleLoopStart, 0.025F, 0.0F, oscillator.sampleLoopEnd - 0.01F, direction); break;
            case 3: oscillator.sampleLoopEnd = stepped(oscillator.sampleLoopEnd, 0.025F, oscillator.sampleLoopStart + 0.01F, 1.0F, direction); break;
            case 4: oscillator.sampleLoop = !oscillator.sampleLoop; break;
            case 5: oscillator.sampleReverse = !oscillator.sampleReverse; break;
            case 6: oscillator.sampleKeyTrack = !oscillator.sampleKeyTrack; break;
            case 7: oscillator.sampleVelocityToGain = stepped(oscillator.sampleVelocityToGain, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: oscillator.sampleOneShot = !oscillator.sampleOneShot; break;
            case 9: preset.sampleBank.rootNote = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.sampleBank.rootNote) + direction, 0, 127)); break;
            default: return;
        }
    } else if (oscillator.waveform == audio::OscillatorWaveform::Granular) {
        switch (index) {
            case 0: oscillator.grainPosition = stepped(oscillator.grainPosition, 0.025F, 0.0F, 1.0F, direction); break;
            case 1: oscillator.grainSizeMilliseconds = stepped(oscillator.grainSizeMilliseconds, 5.0F, 5.0F, 500.0F, direction); break;
            case 2: oscillator.grainDensityHertz = stepped(oscillator.grainDensityHertz, 1.0F, 0.5F, 120.0F, direction); break;
            case 3: oscillator.grainSpray = stepped(oscillator.grainSpray, 0.025F, 0.0F, 1.0F, direction); break;
            case 4: oscillator.grainPitchSemitones = stepped(oscillator.grainPitchSemitones, 1.0F, -48.0F, 48.0F, direction); break;
            case 5: oscillator.grainStereoSpread = stepped(oscillator.grainStereoSpread, 0.05F, 0.0F, 1.0F, direction); break;
            case 6: oscillator.grainFreeze = !oscillator.grainFreeze; break;
            case 7: oscillator.grainWindow = cycle_enum(oscillator.grainWindow, 3U, direction); break;
            case 8: oscillator.sampleReverse = !oscillator.sampleReverse; break;
            case 9: oscillator.sampleKeyTrack = !oscillator.sampleKeyTrack; break;
            default: return;
        }
    } else {
        switch (index) {
            case 0: oscillator.pwmRateHertz = stepped(oscillator.pwmRateHertz, 0.1F, 0.01F, 40.0F, direction); break;
            case 1:
                if (oscillator.waveform == audio::OscillatorWaveform::Wavetable)
                    oscillator.wavetablePosition = stepped(oscillator.wavetablePosition, 0.05F, 0.0F, 1.0F, direction);
                else oscillator.shape = stepped(oscillator.shape, 0.05F, 0.0F, 1.0F, direction);
                break;
            case 2: oscillator.hardSyncSource = cycle_oscillator_source(oscillator.hardSyncSource, direction); break;
            case 3: oscillator.frequencyModSource = cycle_oscillator_source(oscillator.frequencyModSource, direction); break;
            case 4: oscillator.frequencyModMode = cycle_enum(oscillator.frequencyModMode, 3U, direction); break;
            case 5: oscillator.frequencyModAmount = stepped(oscillator.frequencyModAmount, 0.1F, -4.0F, 4.0F, direction); break;
            case 6: oscillator.ringModSource = cycle_oscillator_source(oscillator.ringModSource, direction); break;
            case 7: oscillator.ringModDepth = stepped(oscillator.ringModDepth, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: oscillator.subOscillatorLevel = stepped(oscillator.subOscillatorLevel, 0.05F, 0.0F, 1.0F, direction); break;
            case 9:
                oscillator.subOscillatorOctaves = static_cast<std::uint8_t>(
                    std::clamp<int>(static_cast<int>(oscillator.subOscillatorOctaves) + direction, 1, 3));
                break;
            default: return;
        }
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_macro(std::size_t index, int direction,
                                    audio::Synthesizer& synth) noexcept {
    if (index >= audio::kSynthMacroCount) return;
    auto preset = synth.preset();
    preset.macros.values[index] = stepped(preset.macros.values[index], 0.025F, 0.0F, 1.0F, direction);
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_parameter(std::size_t index, int direction,
                                        audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    if (page_ == SynthPanelPage::FilterEnvelope) {
        switch (index) {
            case 0: preset.filter.topology = cycle_enum(preset.filter.topology, 4U, direction); break;
            case 1: preset.filter.mode = cycle_enum(preset.filter.mode, 4U, direction); break;
            case 2: preset.filter.cutoffHertz = std::clamp(preset.filter.cutoffHertz * (direction > 0 ? 1.18F : 1.0F / 1.18F), 18.0F, 22000.0F); break;
            case 3: preset.filter.resonance = stepped(preset.filter.resonance, 0.05F, 0.0F, 1.0F, direction); break;
            case 4: preset.filter.drive = stepped(preset.filter.drive, 0.15F, 0.1F, 12.0F, direction); break;
            case 5: preset.filter.envelopeAmountOctaves = stepped(preset.filter.envelopeAmountOctaves, 0.25F, -10.0F, 10.0F, direction); break;
            case 6: preset.filter.bassCompensation = stepped(preset.filter.bassCompensation, 0.05F, 0.0F, 1.0F, direction); break;
            case 7: preset.filter.morph = stepped(preset.filter.morph, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: preset.filter.oversampling = cycle_oversampling(preset.filter.oversampling, direction); break;
            case 9: preset.filter.ms20HighPassCutoffHertz = std::clamp(preset.filter.ms20HighPassCutoffHertz * (direction > 0 ? 1.20F : 1.0F / 1.20F), 18.0F, 22000.0F); break;
            case 10: preset.filter.selfOscillation = stepped(preset.filter.selfOscillation, 0.05F, 0.0F, 1.5F, direction); break;
            case 11: preset.filter.keyTrack = stepped(preset.filter.keyTrack, 0.05F, 0.0F, 2.0F, direction); break;
            case 12: preset.ampEnvelope.delaySeconds = stepped(preset.ampEnvelope.delaySeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 13: preset.ampEnvelope.attackSeconds = stepped(preset.ampEnvelope.attackSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 14: preset.ampEnvelope.holdSeconds = stepped(preset.ampEnvelope.holdSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 15: preset.ampEnvelope.decaySeconds = stepped(preset.ampEnvelope.decaySeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 16: preset.ampEnvelope.sustainLevel = stepped(preset.ampEnvelope.sustainLevel, 0.05F, 0.0F, 1.0F, direction); break;
            case 17: preset.ampEnvelope.releaseSeconds = stepped(preset.ampEnvelope.releaseSeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 18: preset.filter.envelope.delaySeconds = stepped(preset.filter.envelope.delaySeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 19: preset.filter.envelope.attackSeconds = stepped(preset.filter.envelope.attackSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 20: preset.filter.envelope.holdSeconds = stepped(preset.filter.envelope.holdSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 21: preset.filter.envelope.decaySeconds = stepped(preset.filter.envelope.decaySeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 22: preset.filter.envelope.sustainLevel = stepped(preset.filter.envelope.sustainLevel, 0.05F, 0.0F, 1.0F, direction); break;
            case 23: preset.filter.envelope.releaseSeconds = stepped(preset.filter.envelope.releaseSeconds, 0.02F, 0.0F, 60.0F, direction); break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Modulation) {
        auto adjust_lfo = [&](std::size_t lfo, std::size_t property) {
            auto& value = preset.lfos[lfo];
            switch (property) {
                case 1: value.waveform = cycle_enum(value.waveform, 6U, direction); break;
                case 2: value.rateHertz = stepped(value.rateHertz, 0.1F, 0.01F, 100.0F, direction); break;
                case 3: value.depth = stepped(value.depth, 0.05F, 0.0F, 1.0F, direction); break;
                case 4: value.phase = stepped(value.phase, 0.05F, 0.0F, 1.0F, direction); break;
                case 5: value.fadeInSeconds = stepped(value.fadeInSeconds, 0.05F, 0.0F, 20.0F, direction); break;
                case 8: value.beatsPerCycle = stepped(value.beatsPerCycle, 0.25F, 0.0625F, 32.0F, direction); break;
                default: break;
            }
        };
        if (index <= 8U) adjust_lfo(0U, index);
        else if (index <= 17U) adjust_lfo(1U, index - 9U);
        else {
            auto& slot = preset.modulation[selectedModulationSlot_];
            switch (index) {
                case 18:
                    selectedModulationSlot_ = static_cast<std::size_t>(
                        (static_cast<int>(selectedModulationSlot_) + direction +
                         static_cast<int>(audio::kSynthModulationSlotCount)) %
                        static_cast<int>(audio::kSynthModulationSlotCount));
                    break;
                case 19: slot.source = cycle_enum(slot.source, 14U, direction); break;
                case 20: slot.destination = cycle_enum(slot.destination, 39U, direction); break;
                case 21: slot.amount = stepped(slot.amount, 0.05F, -1.0F, 1.0F, direction); break;
                case 22: slot.curve = cycle_enum(slot.curve, 3U, direction); break;
                default: break;
            }
        }
    } else if (page_ == SynthPanelPage::Performance) {
        switch (index) {
            case 0: preset.tuning.referenceHertz = stepped(preset.tuning.referenceHertz, 0.5F, 400.0F, 480.0F, direction); break;
            case 1: preset.tuning.transposeSemitones = stepped(preset.tuning.transposeSemitones, 1.0F, -48.0F, 48.0F, direction); break;
            case 2: preset.tuning.fineCents = stepped(preset.tuning.fineCents, 1.0F, -100.0F, 100.0F, direction); break;
            case 3: preset.tuning.analogDriftCents = stepped(preset.tuning.analogDriftCents, 0.1F, 0.0F, 12.0F, direction); break;
            case 5: preset.chord.type = cycle_enum(preset.chord.type, 16U, direction); break;
            case 6: preset.chord.inversion = static_cast<std::int8_t>(std::clamp<int>(preset.chord.inversion + direction, -7, 7)); break;
            case 7: preset.chord.spreadOctaves = static_cast<std::uint8_t>(std::clamp<int>(preset.chord.spreadOctaves + direction, 0, 4)); break;
            case 8: preset.chord.scale = cycle_enum(preset.chord.scale, 7U, direction); break;
            case 9: preset.chord.scaleRoot = static_cast<std::uint8_t>((static_cast<int>(preset.chord.scaleRoot) + direction + 12) % 12); break;
            case 10: preset.chord.strumMilliseconds = stepped(preset.chord.strumMilliseconds, 2.0F, 0.0F, 250.0F, direction); break;
            case 11: preset.chord.velocityScale = stepped(preset.chord.velocityScale, 0.05F, 0.0F, 2.0F, direction); break;
            case 13: preset.arpeggiator.mode = cycle_enum(preset.arpeggiator.mode, 7U, direction); break;
            case 14: preset.arpeggiator.division = cycle_enum(preset.arpeggiator.division, 6U, direction); break;
            case 15: preset.arpeggiator.tempoBpm = stepped(preset.arpeggiator.tempoBpm, 1.0F, 20.0F, 400.0F, direction); break;
            case 16: preset.arpeggiator.clockSource = cycle_enum(preset.arpeggiator.clockSource, 3U, direction); break;
            case 17: preset.arpeggiator.externalTempoBpm = stepped(preset.arpeggiator.externalTempoBpm, 1.0F, 20.0F, 400.0F, direction); break;
            case 18: preset.arpeggiator.gate = stepped(preset.arpeggiator.gate, 0.03F, 0.02F, 1.0F, direction); break;
            case 19: preset.arpeggiator.swing = stepped(preset.arpeggiator.swing, 0.025F, 0.0F, 0.75F, direction); break;
            case 20: preset.arpeggiator.octaveRange = static_cast<std::uint8_t>(std::clamp<int>(preset.arpeggiator.octaveRange + direction, 1, 4)); break;
            case 21: preset.arpeggiator.stepCount = static_cast<std::uint8_t>(std::clamp<int>(preset.arpeggiator.stepCount + direction, 1, 16)); break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Expression) {
        switch (index) {
            case 0: preset.mpe.zoneMode = cycle_enum(preset.mpe.zoneMode, 4U, direction); break;
            case 1: preset.mpe.lowerMasterChannel = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.lowerMasterChannel) + direction, 0, 15)); break;
            case 2: preset.mpe.lowerMemberCount = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.lowerMemberCount) + direction, 0, 15)); break;
            case 3: preset.mpe.upperMasterChannel = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.upperMasterChannel) + direction, 0, 15)); break;
            case 4: preset.mpe.upperMemberCount = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.upperMemberCount) + direction, 0, 15)); break;
            case 5: preset.mpe.masterPitchBendRangeSemitones = stepped(preset.mpe.masterPitchBendRangeSemitones, 1.0F, 0.0F, 96.0F, direction); break;
            case 6: preset.mpe.memberPitchBendRangeSemitones = stepped(preset.mpe.memberPitchBendRangeSemitones, 1.0F, 0.0F, 96.0F, direction); break;
            case 7: preset.mpe.timbreController = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.timbreController) + direction, 0, 127)); break;
            case 10: preset.microtuning.referenceHertz = stepped(preset.microtuning.referenceHertz, 0.5F, 300.0F, 600.0F, direction); break;
            case 11: preset.microtuning.referenceNote = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.microtuning.referenceNote) + direction, 0, 127)); break;
            case 13: preset.unison.voices = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.unison.voices) + direction, 1, static_cast<int>(audio::kSynthUnisonMax))); break;
            case 14: preset.unison.detuneCents = stepped(preset.unison.detuneCents, 0.5F, 0.0F, 100.0F, direction); break;
            case 15: preset.unison.stereoSpread = stepped(preset.unison.stereoSpread, 0.05F, 0.0F, 1.0F, direction); break;
            case 16: preset.unison.phaseSpread = stepped(preset.unison.phaseSpread, 0.025F, 0.0F, 1.0F, direction); break;
            case 18: preset.oscillatorQuality = cycle_enum(preset.oscillatorQuality, 3U, direction); break;
            case 19: preset.filterQuality = cycle_enum(preset.filterQuality, 4U, direction); break;
            case 20:
                selectedModulationSlot_ = static_cast<std::size_t>(
                    (static_cast<int>(selectedModulationSlot_) + direction +
                     static_cast<int>(audio::kSynthModulationSlotCount)) %
                    static_cast<int>(audio::kSynthModulationSlotCount));
                break;
            case 21: preset.modulation[selectedModulationSlot_].polarity =
                cycle_enum(preset.modulation[selectedModulationSlot_].polarity, 2U, direction); break;
            case 22: preset.modulation[selectedModulationSlot_].smoothingMilliseconds =
                stepped(preset.modulation[selectedModulationSlot_].smoothingMilliseconds, 1.0F, 0.0F, 500.0F, direction); break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Presets) {
        auto& mapping = preset.midiLearn[selectedMidiLearnMapping_];
        switch (index) {
            case 0:
                selectedMidiLearnMapping_ = static_cast<std::size_t>(
                    (static_cast<int>(selectedMidiLearnMapping_) + direction +
                     static_cast<int>(audio::kSynthMidiLearnCount)) %
                    static_cast<int>(audio::kSynthMidiLearnCount));
                break;
            case 2: mapping.controller = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(mapping.controller) + direction, 0, 127)); break;
            case 3: mapping.macroIndex = static_cast<std::uint8_t>((static_cast<int>(mapping.macroIndex) + direction + static_cast<int>(audio::kSynthMacroCount)) % static_cast<int>(audio::kSynthMacroCount)); break;
            case 4: mapping.minimum = stepped(mapping.minimum, 0.05F, 0.0F, 1.0F, direction); break;
            case 5: mapping.maximum = stepped(mapping.maximum, 0.05F, 0.0F, 1.0F, direction); break;
            default: break;
        }
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_parameter(std::size_t index, audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    if (page_ == SynthPanelPage::FilterEnvelope) {
        if (index == 0U) preset.filter.enabled = !preset.filter.enabled;
        else if (index == 1U) preset.filter.alternateRevision = !preset.filter.alternateRevision;
    } else if (page_ == SynthPanelPage::Modulation) {
        if (index == 0U) preset.lfos[0].enabled = !preset.lfos[0].enabled;
        else if (index == 6U) preset.lfos[0].keySync = !preset.lfos[0].keySync;
        else if (index == 7U) preset.lfos[0].tempoSync = !preset.lfos[0].tempoSync;
        else if (index == 9U) preset.lfos[1].enabled = !preset.lfos[1].enabled;
        else if (index == 15U) preset.lfos[1].keySync = !preset.lfos[1].keySync;
        else if (index == 16U) preset.lfos[1].tempoSync = !preset.lfos[1].tempoSync;
        else if (index == 23U) {
            auto& slot = preset.modulation[selectedModulationSlot_];
            slot.enabled = !slot.enabled;
        }
    } else if (page_ == SynthPanelPage::Performance) {
        if (index == 4U) preset.chord.enabled = !preset.chord.enabled;
        else if (index == 12U) preset.arpeggiator.enabled = !preset.arpeggiator.enabled;
        else if (index == 22U) preset.arpeggiator.latch = !preset.arpeggiator.latch;
        else if (index == 23U) preset.arpeggiator.retriggerEnvelopes = !preset.arpeggiator.retriggerEnvelopes;
    } else if (page_ == SynthPanelPage::Expression) {
        if (index == 8U) preset.mpe.masterSustainToMembers = !preset.mpe.masterSustainToMembers;
        else if (index == 9U) preset.microtuning.enabled = !preset.microtuning.enabled;
        else if (index == 12U) preset.unison.enabled = !preset.unison.enabled;
        else if (index == 17U) preset.unison.preserveLevel = !preset.unison.preserveLevel;
        else if (index == 23U) preset.metadata.favorite = !preset.metadata.favorite;
    } else if (page_ == SynthPanelPage::Presets) {
        auto& mapping = preset.midiLearn[selectedMidiLearnMapping_];
        if (index == 1U) mapping.enabled = !mapping.enabled;
        else if (index == 6U) mapping.inverted = !mapping.inverted;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_arpeggiator_step_parameter(std::size_t index, int direction,
                                                          audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    auto& step = preset.arpeggiator.steps[selectedArpeggiatorStep_];
    switch (index) {
        case 1: step.condition = cycle_enum(step.condition, 6U, direction); break;
        case 2: step.automationCurve = cycle_enum(step.automationCurve, 3U, direction); break;
        case 5: step.transpose = static_cast<std::int8_t>(std::clamp<int>(step.transpose + direction, -48, 48)); break;
        case 6: step.octaveOffset = static_cast<std::int8_t>(std::clamp<int>(step.octaveOffset + direction, -4, 4)); break;
        case 7: step.velocityScale = stepped(step.velocityScale, 0.05F, 0.0F, 2.0F, direction); break;
        case 8: step.gateScale = stepped(step.gateScale, 0.05F, 0.1F, 2.0F, direction); break;
        case 9: step.probability = stepped(step.probability, 0.05F, 0.0F, 1.0F, direction); break;
        case 10: step.ratchets = static_cast<std::uint8_t>(std::clamp<int>(step.ratchets + direction, 1, 8)); break;
        case 12: step.macro1 = stepped(step.macro1, 0.05F, -1.0F, 1.0F, direction); break;
        case 13: step.macro2 = stepped(step.macro2, 0.05F, -1.0F, 1.0F, direction); break;
        case 14: step.macro3 = stepped(step.macro3, 0.05F, -1.0F, 1.0F, direction); break;
        case 15: step.macro4 = stepped(step.macro4, 0.05F, -1.0F, 1.0F, direction); break;
        default: break;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_arpeggiator_step_parameter(std::size_t index,
                                                          audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    auto& step = preset.arpeggiator.steps[selectedArpeggiatorStep_];
    if (index == 0U) step.enabled = !step.enabled;
    else if (index == 3U) step.accent = !step.accent;
    else if (index == 4U) step.slide = !step.slide;
    else if (index == 11U) step.tie = !step.tie;
    else return;
    synth.set_preset(preset);
}

void EditorSynthPanel::load_selected_preset(audio::Synthesizer& synth) noexcept {
    try {
        if (selectedPresetEntry_ >= presetLibrary_.entries().size()) {
            presetStatus_ = "No preset selected";
            return;
        }
        std::string error;
        auto preset = audio::SynthPreset::load(presetLibrary_.entries()[selectedPresetEntry_].path, &error);
        if (!preset) {
            presetStatus_ = error.empty() ? "Preset load failed" : error;
            return;
        }
        synth.set_preset(*preset);
        presetStatus_ = "Loaded " + preset->name;
    } catch (...) {
        presetStatus_ = "Preset load failed";
    }
}

void EditorSynthPanel::apply_preset_morph(audio::Synthesizer& synth) noexcept {
    if (!compareA_ || !compareB_) {
        presetStatus_ = "Capture A and B before morphing";
        return;
    }
    try {
        synth.set_preset(audio::morph_synth_presets(*compareA_, *compareB_, presetMorphAmount_));
        presetStatus_ = "Applied A/B morph";
    } catch (...) {
        presetStatus_ = "Preset morph failed";
    }
}

void EditorSynthPanel::draw_wavetable_point(int x, int y, audio::Synthesizer& synth) noexcept {
    if (!layout_.wavetableCanvas.contains(x, y)) return;
    auto preset = synth.preset();
    preset.wavetable.enabled = true;
    preset.wavetable.frameCount = std::max<std::uint8_t>(preset.wavetable.frameCount,
        static_cast<std::uint8_t>(selectedWavetableFrame_ + 1U));
    const float normalizedX = static_cast<float>(x - layout_.wavetableCanvas.x) /
                              static_cast<float>(std::max(1, layout_.wavetableCanvas.width - 1));
    const int sample = std::clamp(static_cast<int>(std::lround(normalizedX *
        static_cast<float>(audio::kWavetableSampleCount - 1U))), 0,
        static_cast<int>(audio::kWavetableSampleCount - 1U));
    const float value = std::clamp(1.0F - 2.0F * static_cast<float>(y - layout_.wavetableCanvas.y) /
                                   static_cast<float>(std::max(1, layout_.wavetableCanvas.height - 1)), -1.0F, 1.0F);
    auto& samples = preset.wavetable.samples;
    const std::size_t base = selectedWavetableFrame_ * audio::kWavetableSampleCount;
    if (wavetableLastSample_ >= 0 && wavetableLastSample_ != sample) {
        const int begin = std::min(wavetableLastSample_, sample);
        const int end = std::max(wavetableLastSample_, sample);
        for (int i = begin; i <= end; ++i) {
            const float t = sample == wavetableLastSample_ ? 1.0F :
                static_cast<float>(i - wavetableLastSample_) / static_cast<float>(sample - wavetableLastSample_);
            samples[base + static_cast<std::size_t>(i)] = std::clamp(
                wavetableLastValue_ + (value - wavetableLastValue_) * t, -1.0F, 1.0F);
        }
    } else samples[base + static_cast<std::size_t>(sample)] = value;
    wavetableLastSample_ = sample;
    wavetableLastValue_ = value;
    synth.set_preset(preset);
}

bool EditorSynthPanel::pointer_move(int x, int y, audio::Synthesizer& synth) noexcept {
    if (!open_ || !wavetableDrawing_) return false;
    draw_wavetable_point(x, y, synth);
    return true;
}

bool EditorSynthPanel::pointer_down(int x, int y, audio::Synthesizer& synth) noexcept {
    if (!open_ || !contains(layout_.panel, x, y)) return false;
    if (contains(layout_.closeButton, x, y)) { set_open(false, synth); return true; }
    if (contains(layout_.panicButton, x, y)) { synth.all_notes_off(true); release_panel_notes(synth); return true; }
    if (contains(layout_.resetButton, x, y)) { synth.set_preset(audio::SynthPreset::make_default()); return true; }
    if (contains(layout_.octaveDownButton, x, y)) { octave_ = std::max(0, octave_ - 1); return true; }
    if (contains(layout_.octaveUpButton, x, y)) { octave_ = std::min(8, octave_ + 1); return true; }
    if (contains(layout_.midiThruButton, x, y)) {
        auto preset = synth.preset(); preset.midiThru = !preset.midiThru; synth.set_preset(preset); return true;
    }
    for (std::size_t i = 0; i < layout_.tabButtons.size(); ++i) {
        if (contains(layout_.tabButtons[i], x, y)) { page_ = static_cast<SynthPanelPage>(i); return true; }
    }

    if (page_ == SynthPanelPage::Oscillators) {
        const auto waveform = synth.preset().oscillators[selectedOscillator_].waveform;
        if (waveform == audio::OscillatorWaveform::Wavetable) {
            if (layout_.wavetableCanvas.contains(x, y)) {
                wavetableDrawing_ = true; wavetableLastSample_ = -1;
                draw_wavetable_point(x, y, synth); return true;
            }
            for (std::size_t i = 0; i < layout_.wavetableFrameButtons.size(); ++i)
                if (layout_.wavetableFrameButtons[i].contains(x, y)) { selectedWavetableFrame_ = i; return true; }
            if (layout_.wavetableNormalizeButton.contains(x, y)) { auto preset=synth.preset(); (void)audio::wavetable_normalize(preset.wavetable); synth.set_preset(preset); return true; }
            if (layout_.wavetableRemoveDcButton.contains(x, y)) { auto preset=synth.preset(); (void)audio::wavetable_remove_dc(preset.wavetable); synth.set_preset(preset); return true; }
            if (layout_.wavetableAlignButton.contains(x, y)) { auto preset=synth.preset(); (void)audio::wavetable_align_phases(preset.wavetable); synth.set_preset(preset); return true; }
        }
        for (std::size_t i = 0; i < layout_.oscillatorRows.size(); ++i) {
            if (!contains(layout_.oscillatorRows[i], x, y)) continue;
            selectedOscillator_ = i;
            auto preset = synth.preset(); auto& oscillator = preset.oscillators[i];
            if (contains(layout_.oscillatorEnableButtons[i], x, y)) oscillator.enabled = !oscillator.enabled;
            else if (contains(layout_.oscillatorWaveButtons[i], x, y)) { cycle_waveform(i, 1, synth); return true; }
            else if (contains(layout_.oscillatorGainDownButtons[i], x, y)) oscillator.gain = stepped(oscillator.gain, 0.025F, 0.0F, 2.0F, -1);
            else if (contains(layout_.oscillatorGainUpButtons[i], x, y)) oscillator.gain = stepped(oscillator.gain, 0.025F, 0.0F, 2.0F, 1);
            else if (contains(layout_.oscillatorTuneDownButtons[i], x, y)) oscillator.semitones = stepped(oscillator.semitones, 1.0F, -96.0F, 96.0F, -1);
            else if (contains(layout_.oscillatorTuneUpButtons[i], x, y)) oscillator.semitones = stepped(oscillator.semitones, 1.0F, -96.0F, 96.0F, 1);
            else if (contains(layout_.oscillatorFineDownButtons[i], x, y)) oscillator.cents = stepped(oscillator.cents, 1.0F, -100.0F, 100.0F, -1);
            else if (contains(layout_.oscillatorFineUpButtons[i], x, y)) oscillator.cents = stepped(oscillator.cents, 1.0F, -100.0F, 100.0F, 1);
            else if (contains(layout_.oscillatorPwmDownButtons[i], x, y)) oscillator.pwmDepth = stepped(oscillator.pwmDepth, 0.05F, 0.0F, 1.0F, -1);
            else if (contains(layout_.oscillatorPwmUpButtons[i], x, y)) oscillator.pwmDepth = stepped(oscillator.pwmDepth, 0.05F, 0.0F, 1.0F, 1);
            synth.set_preset(preset); return true;
        }
        for (std::size_t i = 0; i < layout_.oscillatorAdvancedRows.size(); ++i) {
            if (!contains(layout_.oscillatorAdvancedRows[i], x, y)) continue;
            if (contains(layout_.oscillatorAdvancedDownButtons[i], x, y)) adjust_oscillator_advanced(i, -1, synth);
            else if (contains(layout_.oscillatorAdvancedUpButtons[i], x, y)) adjust_oscillator_advanced(i, 1, synth);
            return true;
        }
    } else if (page_ == SynthPanelPage::Effects) {
        for (std::size_t i = 0; i < layout_.effectToggleButtons.size(); ++i)
            if (contains(layout_.effectToggleButtons[i], x, y)) { toggle_effect(i, synth); return true; }
    } else if (page_ == SynthPanelPage::Presets) {
        if (contains(layout_.presetScanButton, x, y)) { (void)refresh_preset_library(); return true; }
        if (contains(layout_.presetPreviousButton, x, y)) {
            if (!presetLibrary_.entries().empty()) selectedPresetEntry_ =
                (selectedPresetEntry_ + presetLibrary_.entries().size() - 1U) % presetLibrary_.entries().size();
            return true;
        }
        if (contains(layout_.presetNextButton, x, y)) {
            if (!presetLibrary_.entries().empty()) selectedPresetEntry_ =
                (selectedPresetEntry_ + 1U) % presetLibrary_.entries().size();
            return true;
        }
        if (contains(layout_.presetLoadButton, x, y)) { load_selected_preset(synth); return true; }
        if (contains(layout_.presetCaptureAButton, x, y)) { compareA_ = synth.preset(); presetStatus_ = "Captured A"; return true; }
        if (contains(layout_.presetCaptureBButton, x, y)) { compareB_ = synth.preset(); presetStatus_ = "Captured B"; return true; }
        if (contains(layout_.presetMorphDownButton, x, y)) { presetMorphAmount_ = stepped(presetMorphAmount_, 0.05F, 0.0F, 1.0F, -1); apply_preset_morph(synth); return true; }
        if (contains(layout_.presetMorphUpButton, x, y)) { presetMorphAmount_ = stepped(presetMorphAmount_, 0.05F, 0.0F, 1.0F, 1); apply_preset_morph(synth); return true; }
        for (std::size_t i = 0; i < layout_.presetEntryButtons.size(); ++i) {
            if (contains(layout_.presetEntryButtons[i], x, y) && i < presetLibrary_.entries().size()) {
                selectedPresetEntry_ = i;
                return true;
            }
        }
        for (std::size_t i = 0; i < 7U; ++i) {
            if (!contains(layout_.parameterRows[i], x, y)) continue;
            if (contains(layout_.parameterDownButtons[i], x, y)) adjust_parameter(i, -1, synth);
            else if (contains(layout_.parameterUpButtons[i], x, y)) adjust_parameter(i, 1, synth);
            else if (contains(layout_.parameterToggleButtons[i], x, y)) toggle_parameter(i, synth);
            return true;
        }
    } else {
        for (std::size_t i = 0; i < layout_.parameterRows.size(); ++i) {
            if (!contains(layout_.parameterRows[i], x, y)) continue;
            if (contains(layout_.parameterDownButtons[i], x, y)) adjust_parameter(i, -1, synth);
            else if (contains(layout_.parameterUpButtons[i], x, y)) adjust_parameter(i, 1, synth);
            else if (contains(layout_.parameterToggleButtons[i], x, y)) toggle_parameter(i, synth);
            return true;
        }
        if (page_ == SynthPanelPage::Modulation) {
            for (std::size_t i = 0; i < layout_.macroRows.size(); ++i) {
                if (!contains(layout_.macroRows[i], x, y)) continue;
                if (contains(layout_.macroDownButtons[i], x, y)) adjust_macro(i, -1, synth);
                else if (contains(layout_.macroUpButtons[i], x, y)) adjust_macro(i, 1, synth);
                return true;
            }
        }
        if (page_ == SynthPanelPage::Performance) {
            for (std::size_t i = 0; i < layout_.arpeggiatorStepButtons.size(); ++i) {
                if (contains(layout_.arpeggiatorStepButtons[i], x, y)) { selectedArpeggiatorStep_ = i; return true; }
            }
            for (std::size_t i = 0; i < layout_.arpeggiatorStepRows.size(); ++i) {
                if (!contains(layout_.arpeggiatorStepRows[i], x, y)) continue;
                if (contains(layout_.arpeggiatorStepDownButtons[i], x, y))
                    adjust_arpeggiator_step_parameter(i, -1, synth);
                else if (contains(layout_.arpeggiatorStepUpButtons[i], x, y))
                    adjust_arpeggiator_step_parameter(i, 1, synth);
                else if (contains(layout_.arpeggiatorStepToggleButtons[i], x, y))
                    toggle_arpeggiator_step_parameter(i, synth);
                return true;
            }
        }
    }

    for (std::size_t i = 0; i < layout_.pianoKeys.size(); ++i) {
        if (contains(layout_.pianoKeys[i], x, y)) {
            pointerNote_ = std::clamp(octave_ * 12 + 36 + static_cast<int>(i), 0, 127);
            (void)synth.note_on(static_cast<std::uint8_t>(pointerNote_), 0.85F);
            return true;
        }
    }
    return true;
}

bool EditorSynthPanel::pointer_up(int, int, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    if (wavetableDrawing_) { wavetableDrawing_ = false; wavetableLastSample_ = -1; return true; }
    if (pointerNote_ < 0) return false;
    (void)synth.note_off(static_cast<std::uint8_t>(pointerNote_));
    pointerNote_ = -1;
    return true;
}

int EditorSynthPanel::keyboard_note(std::string_view key, int octave) noexcept {
    static constexpr std::array<std::string_view, 20> keys{
        "z","s","x","d","c","v","g","b","h","n","j","m",",","l",".",";","/","q","2","w"};
    const std::string normalized = lower(key);
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (normalized == keys[i]) return std::clamp(octave * 12 + 24 + static_cast<int>(i), 0, 127);
    return -1;
}

bool EditorSynthPanel::key_down(std::string_view key, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    const std::string normalized = lower(key);
    if (normalized == "escape") { set_open(false, synth); return true; }
    if (normalized == "left") { octave_ = std::max(0, octave_ - 1); return true; }
    if (normalized == "right") { octave_ = std::min(8, octave_ + 1); return true; }
    static constexpr std::array<std::string_view, kSynthPanelPageCount> pageKeys{"1","3","4","5","6","7","8"};
    for (std::size_t i = 0; i < pageKeys.size(); ++i) {
        if (normalized == pageKeys[i] || normalized == "f" + std::to_string(i + 1U)) {
            page_ = static_cast<SynthPanelPage>(i);
            return true;
        }
    }
    const int note = keyboard_note(normalized, octave_);
    if (note < 0) return false;
    if (!keyboardNotes_[static_cast<std::size_t>(note)]) {
        keyboardNotes_[static_cast<std::size_t>(note)] = true;
        (void)synth.note_on(static_cast<std::uint8_t>(note), 0.82F);
    }
    return true;
}

bool EditorSynthPanel::key_up(std::string_view key, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    const int note = keyboard_note(key, octave_);
    if (note < 0) return false;
    if (keyboardNotes_[static_cast<std::size_t>(note)]) {
        keyboardNotes_[static_cast<std::size_t>(note)] = false;
        (void)synth.note_off(static_cast<std::uint8_t>(note));
    }
    return true;
}

} // namespace dve::editor
