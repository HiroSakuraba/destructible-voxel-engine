#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "dve/editor_native.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void click(dve::editor::NativeEditorController& controller, dve::editor::UiRect rect) {
    controller.pointer_down(dve::editor::PointerButton::Primary, rect.x + rect.width / 2, rect.y + rect.height / 2);
    controller.pointer_up(dve::editor::PointerButton::Primary, rect.x + rect.width / 2, rect.y + rect.height / 2);
}
}

int main() {
    try {
        using namespace dve::editor;
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        require(controller.workspace().menus().find("window.toggle_synth") != nullptr,
                "synth menu action was not registered");
        require(controller.dispatch_action("window.toggle_synth"), "synth menu action failed");
        require(controller.synth_panel().open(), "synth panel did not open");

        controller.key_down("z", false, false, false);
        std::vector<float> audio(4096U * 2U);
        controller.synthesizer().render(audio);
        require(controller.synthesizer().meters().activeVoices == 1, "computer keyboard did not trigger synth");
        require(std::any_of(audio.begin(), audio.end(), [](float sample) { return std::abs(sample) > 0.001F; }),
                "editor-triggered synth produced silence");
        controller.key_up("z", false, false, false);

        auto panel = controller.synth_panel().layout();
        const bool beforeEnable = controller.synthesizer().preset().oscillators[0].enabled;
        click(controller, panel.oscillatorEnableButtons[0]);
        require(controller.synthesizer().preset().oscillators[0].enabled != beforeEnable,
                "oscillator enable control did not update preset");
        const auto beforeWave = controller.synthesizer().preset().oscillators[0].waveform;
        click(controller, panel.oscillatorWaveButtons[0]);
        require(controller.synthesizer().preset().oscillators[0].waveform != beforeWave,
                "expanded waveform selector did not update preset");
        const float beforePwm = controller.synthesizer().preset().oscillators[0].pwmDepth;
        click(controller, panel.oscillatorPwmUpButtons[0]);
        require(controller.synthesizer().preset().oscillators[0].pwmDepth > beforePwm,
                "pulse-width modulation control did not update preset");

        click(controller, panel.tabButtons[1]);
        require(controller.synth_panel().page() == SynthPanelPage::FilterEnvelope,
                "filter/envelope page did not open");
        const auto beforeTopology = controller.synthesizer().preset().filter.topology;
        click(controller, panel.parameterUpButtons[0]);
        require(controller.synthesizer().preset().filter.topology != beforeTopology,
                "filter model selector did not update preset");
        const float beforeAttack = controller.synthesizer().preset().ampEnvelope.attackSeconds;
        click(controller, panel.parameterUpButtons[13]);
        require(controller.synthesizer().preset().ampEnvelope.attackSeconds > beforeAttack,
                "amplitude attack control did not update preset");
        const float beforeRelease = controller.synthesizer().preset().filter.envelope.releaseSeconds;
        click(controller, panel.parameterUpButtons[23]);
        require(controller.synthesizer().preset().filter.envelope.releaseSeconds > beforeRelease,
                "filter release control did not update preset");

        click(controller, panel.tabButtons[0]);
        const float beforeFmAmount = controller.synthesizer().preset().oscillators[0].frequencyModAmount;
        click(controller, panel.oscillatorAdvancedUpButtons[5]);
        require(controller.synthesizer().preset().oscillators[0].frequencyModAmount > beforeFmAmount,
                "oscillator FM amount control did not update preset");

        auto wavetablePreset = controller.synthesizer().preset();
        wavetablePreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Wavetable;
        wavetablePreset.wavetable.enabled = true;
        controller.synthesizer().set_preset(wavetablePreset);
        const float beforeDraw = controller.synthesizer().preset().wavetable.samples[64];
        controller.pointer_down(PointerButton::Primary, panel.wavetableCanvas.x + panel.wavetableCanvas.width / 2,
                                panel.wavetableCanvas.y + 4);
        controller.pointer_move(panel.wavetableCanvas.x + panel.wavetableCanvas.width * 3 / 4,
                                panel.wavetableCanvas.y + panel.wavetableCanvas.height - 5);
        controller.pointer_up(PointerButton::Primary, panel.wavetableCanvas.x + panel.wavetableCanvas.width * 3 / 4,
                              panel.wavetableCanvas.y + panel.wavetableCanvas.height - 5);
        require(std::abs(controller.synthesizer().preset().wavetable.samples[64] - beforeDraw) > 0.05F,
                "freehand wavetable canvas did not update the selected frame");
        click(controller, panel.wavetableFrameButtons[3]);
        require(controller.synth_panel().selected_wavetable_frame() == 3U,
                "wavetable frame selector did not update selection");

        auto samplePreset = controller.synthesizer().preset();
        samplePreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Sample;
        samplePreset.sampleBank.enabled = true; samplePreset.sampleBank.frameCount = 4;
        samplePreset.sampleBank.samples[0] = 0.0F; samplePreset.sampleBank.samples[1] = 1.0F;
        samplePreset.sampleBank.samples[2] = -1.0F; samplePreset.sampleBank.samples[3] = 0.0F;
        controller.synthesizer().set_preset(samplePreset);
        const float beforeSampleStart = controller.synthesizer().preset().oscillators[0].sampleStart;
        click(controller, panel.oscillatorAdvancedUpButtons[0]);
        require(controller.synthesizer().preset().oscillators[0].sampleStart > beforeSampleStart,
                "sample oscillator start control did not update preset");
        click(controller, panel.oscillatorAdvancedUpButtons[4]);
        require(controller.synthesizer().preset().oscillators[0].sampleLoop,
                "sample oscillator loop control did not update preset");

        auto granularPreset = controller.synthesizer().preset();
        granularPreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Granular;
        controller.synthesizer().set_preset(granularPreset);
        const float beforeGrainDensity = controller.synthesizer().preset().oscillators[0].grainDensityHertz;
        click(controller, panel.oscillatorAdvancedUpButtons[2]);
        require(controller.synthesizer().preset().oscillators[0].grainDensityHertz > beforeGrainDensity,
                "granular density control did not update preset");

        click(controller, panel.tabButtons[2]);
        require(controller.synth_panel().page() == SynthPanelPage::Modulation,
                "modulation page did not open");
        click(controller, panel.parameterToggleButtons[0]);
        require(controller.synthesizer().preset().lfos[0].enabled,
                "LFO enable control did not update preset");
        const float beforeLfoRate = controller.synthesizer().preset().lfos[0].rateHertz;
        click(controller, panel.parameterUpButtons[2]);
        require(controller.synthesizer().preset().lfos[0].rateHertz > beforeLfoRate,
                "LFO rate control did not update preset");
        click(controller, panel.parameterToggleButtons[23]);
        require(controller.synthesizer().preset().modulation[0].enabled,
                "modulation route enable control did not update preset");
        const float beforeMacro = controller.synthesizer().preset().macros.values[0];
        click(controller, panel.macroUpButtons[0]);
        require(controller.synthesizer().preset().macros.values[0] > beforeMacro,
                "macro control did not update preset");

        click(controller, panel.tabButtons[3]);
        require(controller.synth_panel().page() == SynthPanelPage::Performance,
                "performance page did not open");
        click(controller, panel.parameterToggleButtons[4]);
        click(controller, panel.parameterToggleButtons[12]);
        require(controller.synthesizer().preset().chord.enabled &&
                controller.synthesizer().preset().arpeggiator.enabled,
                "chord/arpeggiator controls did not enable performance engines");
        const auto beforeChord = controller.synthesizer().preset().chord.type;
        click(controller, panel.parameterUpButtons[5]);
        require(controller.synthesizer().preset().chord.type != beforeChord,
                "chord type selector did not update preset");
        const float beforeTempo = controller.synthesizer().preset().arpeggiator.tempoBpm;
        click(controller, panel.parameterUpButtons[15]);
        require(controller.synthesizer().preset().arpeggiator.tempoBpm > beforeTempo,
                "arpeggiator tempo control did not update preset");
        click(controller, panel.arpeggiatorStepButtons[5]);
        require(controller.synth_panel().selected_arpeggiator_step() == 5U,
                "arpeggiator step selector did not update selection");
        const auto beforeStep = controller.synthesizer().preset().arpeggiator.steps[5];
        click(controller, panel.arpeggiatorStepUpButtons[1]);
        click(controller, panel.arpeggiatorStepUpButtons[2]);
        click(controller, panel.arpeggiatorStepToggleButtons[3]);
        click(controller, panel.arpeggiatorStepToggleButtons[4]);
        click(controller, panel.arpeggiatorStepUpButtons[5]);
        click(controller, panel.arpeggiatorStepDownButtons[6]);
        click(controller, panel.arpeggiatorStepUpButtons[7]);
        click(controller, panel.arpeggiatorStepDownButtons[8]);
        click(controller, panel.arpeggiatorStepDownButtons[9]);
        click(controller, panel.arpeggiatorStepUpButtons[10]);
        click(controller, panel.arpeggiatorStepToggleButtons[11]);
        click(controller, panel.arpeggiatorStepUpButtons[14]);
        click(controller, panel.arpeggiatorStepToggleButtons[0]);
        const auto afterStep = controller.synthesizer().preset().arpeggiator.steps[5];
        require(afterStep.condition != beforeStep.condition &&
                afterStep.automationCurve != beforeStep.automationCurve &&
                afterStep.accent != beforeStep.accent && afterStep.slide != beforeStep.slide &&
                afterStep.transpose == beforeStep.transpose + 1 &&
                afterStep.octaveOffset == beforeStep.octaveOffset - 1 &&
                afterStep.velocityScale > beforeStep.velocityScale &&
                afterStep.gateScale < beforeStep.gateScale &&
                afterStep.probability < beforeStep.probability &&
                afterStep.ratchets == beforeStep.ratchets + 1U &&
                afterStep.tie != beforeStep.tie && afterStep.macro3 > beforeStep.macro3 &&
                afterStep.enabled != beforeStep.enabled,
                "arpeggiator per-step editor did not update all step properties");

        click(controller, panel.tabButtons[4]);
        require(controller.synth_panel().page() == SynthPanelPage::Effects, "effects page did not open");
        const bool beforeEffect = controller.synthesizer().preset().distortion.enabled;
        click(controller, panel.effectToggleButtons[0]);
        require(controller.synthesizer().preset().distortion.enabled != beforeEffect,
                "effects page control did not update preset");

        click(controller, panel.tabButtons[6]);
        require(controller.synth_panel().page() == SynthPanelPage::Expression,
                "expression page did not open");
        const auto beforeMpe = controller.synthesizer().preset().mpe.zoneMode;
        click(controller, panel.parameterUpButtons[0]);
        require(controller.synthesizer().preset().mpe.zoneMode != beforeMpe,
                "MPE zone control did not update preset");
        click(controller, panel.parameterToggleButtons[9]);
        require(controller.synthesizer().preset().microtuning.enabled,
                "microtuning control did not update preset");
        click(controller, panel.parameterToggleButtons[12]);
        require(controller.synthesizer().preset().unison.enabled,
                "unison control did not update preset");
        const auto beforeUnisonVoices = controller.synthesizer().preset().unison.voices;
        click(controller, panel.parameterUpButtons[13]);
        require(controller.synthesizer().preset().unison.voices > beforeUnisonVoices,
                "unison voice control did not update preset");
        const auto beforePolarity = controller.synthesizer().preset().modulation[0].polarity;
        click(controller, panel.parameterUpButtons[21]);
        require(controller.synthesizer().preset().modulation[0].polarity != beforePolarity,
                "modulation polarity control did not update preset");

        const auto presetRoot = std::filesystem::temp_directory_path() / "dve_editor_synth_presets";
        std::filesystem::remove_all(presetRoot);
        std::filesystem::create_directories(presetRoot / "Bass");
        std::string presetError;
        auto libraryPreset = controller.synthesizer().preset();
        libraryPreset.name = "Editor Bass";
        require(libraryPreset.save(presetRoot / "Bass" / "editor_bass.dvesynth", &presetError),
                presetError.c_str());
        controller.synth_panel().set_preset_directory(presetRoot);
        click(controller, panel.tabButtons[5]);
        require(controller.synth_panel().page() == SynthPanelPage::Presets, "presets page did not open");
        click(controller, panel.presetScanButton);
        require(controller.synth_panel().preset_library().entries().size() == 1U,
                "preset browser did not index the test preset");
        click(controller, panel.presetEntryButtons[0]);
        click(controller, panel.presetLoadButton);
        require(controller.synthesizer().preset().name == "Editor Bass",
                "preset browser did not load the selected preset");
        click(controller, panel.parameterToggleButtons[1]);
        require(controller.synthesizer().preset().midiLearn[0].enabled,
                "MIDI learn mapping control did not update preset");

        require(controller.dispatch_action("window.toggle_synth"), "synth close action failed");
        require(!controller.synth_panel().open(), "synth panel did not close");
        std::cout << "dve_editor_synth_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_synth_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
