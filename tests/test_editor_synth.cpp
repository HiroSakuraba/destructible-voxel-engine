#include <memory>
#include <utility>
#include <cstdio>
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

// The controller (~300 KB) and each full synth preset copy live on the heap, and the
// scenario is split into one function per synth page: as one function, main's frame
// held every layout and preset temporary at once, which under ASan (redzones, no slot
// reuse) came to ~9 MB and overflowed the 8 MB stack.
template <class T>
std::unique_ptr<T> heap_copy(const T& value) { return std::make_unique<T>(value); }

void oscillator_and_filter_pages(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
    panel = controller.synth_panel().layout();
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
    // Phase 2: Comb/Formant topologies are reachable; comb params are adjustable.
    for (int i = 0; i < 6; ++i) {
        if (controller.synthesizer().preset().filter.topology == dve::audio::FilterTopology::Comb) break;
        click(controller, panel.parameterUpButtons[0]);
    }
    require(controller.synthesizer().preset().filter.topology == dve::audio::FilterTopology::Comb,
            "filter topology cycle never reached Comb");
    const float beforeCombDamping = controller.synthesizer().preset().filter.comb.damping;
    click(controller, panel.parameterUpButtons[24]);
    require(controller.synthesizer().preset().filter.comb.damping > beforeCombDamping,
            "comb damping control did not update preset");

    click(controller, panel.tabButtons[0]);
    const float beforeFmAmount = controller.synthesizer().preset().oscillators[0].frequencyModAmount;
    click(controller, panel.oscillatorAdvancedUpButtons[5]);
    require(controller.synthesizer().preset().oscillators[0].frequencyModAmount > beforeFmAmount,
            "oscillator FM amount control did not update preset");
}

void wavetable_section(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
    auto wavetablePresetOwner = heap_copy(controller.synthesizer().preset());
    auto& wavetablePreset = *wavetablePresetOwner;
    wavetablePreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Wavetable;
    wavetablePreset.wavetable.enabled = true;
    controller.synthesizer().set_preset(wavetablePreset);
    // The wavetable section sits below the advanced rows; at 1280x800 the Oscillators page
    // scrolls to it (it used to be drawn over the voice meter and piano).
    controller.update(0.0F);
    require(controller.synth_panel().wavetable_section_visible(), "wavetable section not shown for a wavetable oscillator");
    while (controller.synth_panel().layout().wavetableNormalizeButton.width == 0 &&
           controller.synth_panel().scroll_grid(1)) {}
    panel = controller.synth_panel().layout();
    require(panel.wavetableCanvas.width > 0 && panel.wavetableFrameButtons[3].width > 0,
            "wavetable canvas could not be scrolled into view");
    const float beforeDraw = controller.synthesizer().preset().wavetable.samples[64];
    controller.pointer_down(PointerButton::Primary, panel.wavetableCanvas.x + panel.wavetableCanvas.width / 2,
                            panel.wavetableCanvas.y + 4);
    controller.pointer_move(panel.wavetableCanvas.x + panel.wavetableCanvas.width * 3 / 4,
                            panel.wavetableCanvas.y + panel.wavetableCanvas.height - 5);
    controller.pointer_up(PointerButton::Primary, panel.wavetableCanvas.x + panel.wavetableCanvas.width * 3 / 4,
                          panel.wavetableCanvas.y + panel.wavetableCanvas.height - 5);
    require(std::abs(controller.synthesizer().preset().wavetable.samples[64] - beforeDraw) > 0.05F,
            "freehand wavetable canvas did not update the selected frame");
    {
        // A fast 200-move stroke must not cook/publish per move; the final
        // stroke state must land on pointer-up.
        const auto publishesBefore = controller.synth_panel().wavetable_draw_publish_count();
        const auto cooksBefore = controller.synthesizer().wavetable_cook_count();
        const int cx = panel.wavetableCanvas.x, cy = panel.wavetableCanvas.y;
        const int cw = panel.wavetableCanvas.width, ch = panel.wavetableCanvas.height;
        controller.pointer_down(PointerButton::Primary, cx + 2, cy + ch / 2);
        for (int i = 0; i < 200; ++i)
            controller.pointer_move(cx + 2 + (cw - 4) * i / 199, cy + 3 + (i % 2) * (ch - 6));
        const int endX = cx + cw - 3, endY = cy + 3;
        controller.pointer_up(PointerButton::Primary, endX, endY);
        const auto publishes = controller.synth_panel().wavetable_draw_publish_count() - publishesBefore;
        const auto cooks = controller.synthesizer().wavetable_cook_count() - cooksBefore;
        std::printf("wavetable stroke: 201 draw events -> %llu publishes, %llu cooks\n",
                    static_cast<unsigned long long>(publishes), static_cast<unsigned long long>(cooks));
        require(publishes >= 1U && publishes < 50U, "wavetable drawing was not coalesced");
        require(cooks <= publishes, "wavetable drawing cooked more than it published");
        require(!controller.synth_panel().wavetable_draw_pending(), "wavetable draft not flushed on pointer-up");
        const auto& samples = controller.synthesizer().preset().wavetable.samples;
        const std::size_t frame = controller.synth_panel().selected_wavetable_frame();
        require(samples[frame * dve::audio::kWavetableSampleCount + dve::audio::kWavetableSampleCount - 2U] > 0.5F,
                "final wavetable stroke point was not applied");
    }
    click(controller, panel.wavetableFrameButtons[3]);
    require(controller.synth_panel().selected_wavetable_frame() == 3U,
            "wavetable frame selector did not update selection");
    (void)controller.synth_panel().scroll_grid(-100);
    panel = controller.synth_panel().layout();
}

void oscillator_sources(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
    auto samplePresetOwner = heap_copy(controller.synthesizer().preset());
    auto& samplePreset = *samplePresetOwner;
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

    auto granularPresetOwner = heap_copy(controller.synthesizer().preset());
    auto& granularPreset = *granularPresetOwner;
    granularPreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Granular;
    controller.synthesizer().set_preset(granularPreset);
    // Phase 4: the Granular waveform drives the preset-level granular
    // generator; the advanced section edits GranularParameters (row 1 =
    // density).
    const float beforeGrainDensity = controller.synthesizer().preset().granular.densityHz;
    click(controller, panel.oscillatorAdvancedUpButtons[1]);
    require(controller.synthesizer().preset().granular.densityHz > beforeGrainDensity,
            "granular density control did not update preset");

    // Phase 2: Sampler and ModalResonator waveforms are reachable through the wave cycle.
    for (int i = 0; i < 16; ++i) {
        if (controller.synthesizer().preset().oscillators[0].waveform ==
            dve::audio::OscillatorWaveform::Sampler) break;
        click(controller, panel.oscillatorWaveButtons[0]);
    }
    require(controller.synthesizer().preset().oscillators[0].waveform ==
            dve::audio::OscillatorWaveform::Sampler,
            "waveform cycle never reached Sampler");
    const float beforeSamplerGain = controller.synthesizer().preset().sampler.gain;
    click(controller, panel.oscillatorAdvancedUpButtons[4]);
    require(controller.synthesizer().preset().sampler.gain > beforeSamplerGain,
            "sampler gain control did not update preset");

    auto modalPresetOwner = heap_copy(controller.synthesizer().preset());
    auto& modalPreset = *modalPresetOwner;
    modalPreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::ModalResonator;
    controller.synthesizer().set_preset(modalPreset);
    const auto beforeExcitation = controller.synthesizer().preset().oscillators[0].modalResonator.excitation;
    click(controller, panel.oscillatorAdvancedUpButtons[0]);
    require(controller.synthesizer().preset().oscillators[0].modalResonator.excitation != beforeExcitation,
            "modal excitation control did not update preset");
    const float beforeDamping = controller.synthesizer().preset().oscillators[0].modalResonator.damping;
    click(controller, panel.oscillatorAdvancedUpButtons[2]);
    require(controller.synthesizer().preset().oscillators[0].modalResonator.damping > beforeDamping,
            "modal damping control did not update preset");

    auto spectralPresetOwner = heap_copy(controller.synthesizer().preset());
    auto& spectralPreset = *spectralPresetOwner;
    spectralPreset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Spectral;
    controller.synthesizer().set_preset(spectralPreset);
    // Phase 5: the Spectral waveform drives the preset-level spectral/
    // resynthesis parameters (row 2 = stretch, row 3 = freeze amount).
    const float beforeStretch = controller.synthesizer().preset().spectral.timeStretch;
    click(controller, panel.oscillatorAdvancedUpButtons[2]);
    require(controller.synthesizer().preset().spectral.timeStretch > beforeStretch,
            "spectral stretch control did not update preset");
    const float beforeFreeze = controller.synthesizer().preset().spectral.freeze01;
    click(controller, panel.oscillatorAdvancedUpButtons[3]);
    require(controller.synthesizer().preset().spectral.freeze01 != beforeFreeze,
            "spectral freeze control did not update preset");
}

void modulation_and_performance_pages(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
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
    // Phase 2: destination cycle reaches SamplerStartPosition; bias row adjusts.
    for (int i = 0; i < 42; ++i) {
        if (controller.synthesizer().preset().modulation[0].destination ==
            dve::audio::ModulationDestination::SamplerStartPosition) break;
        click(controller, panel.parameterUpButtons[20]);
    }
    require(controller.synthesizer().preset().modulation[0].destination ==
            dve::audio::ModulationDestination::SamplerStartPosition,
            "modulation destination cycle never reached SamplerStartPosition");
    const float beforeBias = controller.synthesizer().preset().modulation[0].bias;
    click(controller, panel.parameterUpButtons[24]);
    require(controller.synthesizer().preset().modulation[0].bias > beforeBias,
            "modulation bias control did not update preset");
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
    click(controller, panel.arpeggiatorStepUpButtons[4]);
    click(controller, panel.arpeggiatorStepToggleButtons[5]);
    click(controller, panel.arpeggiatorStepToggleButtons[6]);
    click(controller, panel.arpeggiatorStepUpButtons[7]);
    click(controller, panel.arpeggiatorStepDownButtons[8]);
    click(controller, panel.arpeggiatorStepUpButtons[9]);
    click(controller, panel.arpeggiatorStepDownButtons[10]);
    click(controller, panel.arpeggiatorStepDownButtons[11]);
    click(controller, panel.arpeggiatorStepUpButtons[12]);
    click(controller, panel.arpeggiatorStepToggleButtons[13]);
    click(controller, panel.arpeggiatorStepUpButtons[16]);
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
}

void effects_page(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
    click(controller, panel.tabButtons[4]);
    require(controller.synth_panel().page() == SynthPanelPage::Effects, "effects page did not open");
    panel = controller.synth_panel().layout();  // each page lays out its own rows
    const bool beforeEffect = controller.synthesizer().preset().distortion.enabled;
    click(controller, panel.effectToggleButtons[0]);
    require(controller.synthesizer().preset().distortion.enabled != beforeEffect,
            "effects page control did not update preset");

    // FX parameter editor: select an effect row, adjust its parameters.
    click(controller, panel.effectRows[5]); // Flanger
    require(controller.synth_panel().selected_effect() == 5U, "effect row did not select the effect");
    const float beforeFlangerRate = controller.synthesizer().preset().flanger.rateHertz;
    click(controller, panel.effectParamUpButtons[0]);
    require(controller.synthesizer().preset().flanger.rateHertz > beforeFlangerRate,
            "flanger rate control did not update preset");
    const float beforeFlangerMix = controller.synthesizer().preset().flanger.mix;
    click(controller, panel.effectParamDownButtons[3]);
    require(controller.synthesizer().preset().flanger.mix < beforeFlangerMix,
            "flanger mix control did not update preset");

    click(controller, panel.effectRows[0]); // Distortion: toggle param (Classic/Fuzz)
    require(controller.synth_panel().selected_effect() == 0U, "effect row did not select distortion");
    const auto beforeMode = controller.synthesizer().preset().distortion.mode;
    click(controller, panel.effectParamToggleButtons[2]);
    require(controller.synthesizer().preset().distortion.mode != beforeMode,
            "distortion mode toggle did not update preset");

    // Phase 2: distortion mode cycles through all four modes.
    const auto modeStart = controller.synthesizer().preset().distortion.mode;
    click(controller, panel.effectParamToggleButtons[2]);
    const auto modeNext = controller.synthesizer().preset().distortion.mode;
    require(modeNext != modeStart, "distortion mode toggle did not advance");
    click(controller, panel.effectParamToggleButtons[2]);
    click(controller, panel.effectParamToggleButtons[2]);
    require(controller.synthesizer().preset().distortion.mode != modeNext,
            "distortion mode toggle did not reach a third mode");
    click(controller, panel.effectParamToggleButtons[2]);
    require(controller.synthesizer().preset().distortion.mode == modeStart,
            "distortion mode toggle did not wrap after four modes");

    // Phase 2: delay tempo sync toggle + sync beats.
    click(controller, panel.effectRows[8]); // Delay
    require(controller.synth_panel().selected_effect() == 8U, "effect row did not select delay");
    const bool beforeTempoSync = controller.synthesizer().preset().delay.tempoSync;
    click(controller, panel.effectParamToggleButtons[4]);
    require(controller.synthesizer().preset().delay.tempoSync != beforeTempoSync,
            "delay tempo sync toggle did not update preset");
    const float beforeSyncBeats = controller.synthesizer().preset().delay.syncBeats;
    click(controller, panel.effectParamUpButtons[5]);
    require(controller.synthesizer().preset().delay.syncBeats > beforeSyncBeats,
            "delay sync beats control did not update preset");

    // Phase 2: diffusion delay row, enable toggle, and parameters.
    click(controller, panel.effectRows[12]); // Diffusion
    require(controller.synth_panel().selected_effect() == 12U, "effect row did not select diffusion");
    const bool beforeDiffusion = controller.synthesizer().preset().diffusionDelay.enabled;
    click(controller, panel.effectToggleButtons[12]);
    require(controller.synthesizer().preset().diffusionDelay.enabled != beforeDiffusion,
            "diffusion enable control did not update preset");
    const float beforeDiffusionTime = controller.synthesizer().preset().diffusionDelay.timeSeconds;
    click(controller, panel.effectParamUpButtons[0]);
    require(controller.synthesizer().preset().diffusionDelay.timeSeconds > beforeDiffusionTime,
            "diffusion time control did not update preset");
    require(controller.synthesizer().preset().validate(), "preset invalid after Phase 2 FX edits");

    click(controller, panel.effectRows[10]); // Compressor: 5 params
    const float beforeThreshold = controller.synthesizer().preset().compressor.thresholdDb;
    click(controller, panel.effectParamUpButtons[0]);
    require(controller.synthesizer().preset().compressor.thresholdDb > beforeThreshold,
            "compressor threshold control did not update preset");
    require(controller.synthesizer().preset().validate(), "preset invalid after FX parameter edits");
}

void expression_page(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
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
}

void presets_and_search(dve::editor::NativeEditorController& controller, dve::editor::SynthPanelLayout& panel) {
    using namespace dve::editor;
    const auto presetRoot = std::filesystem::temp_directory_path() / "dve_editor_synth_presets";
    std::filesystem::remove_all(presetRoot);
    std::filesystem::create_directories(presetRoot / "Bass");
    std::string presetError;
    auto libraryPresetOwner = heap_copy(controller.synthesizer().preset());
    auto& libraryPreset = *libraryPresetOwner;
    libraryPreset.name = "Editor Bass";
    require(libraryPreset.save(presetRoot / "Bass" / "editor_bass.dvesynth", &presetError),
            presetError.c_str());
    controller.synth_panel().set_preset_directory(presetRoot);
    click(controller, panel.tabButtons[5]);
    require(controller.synth_panel().page() == SynthPanelPage::Presets, "presets page did not open");
    panel = controller.synth_panel().layout();  // each page lays out its own rows
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

    // Phase 6: patch-search browser panel wiring.
    require(!controller.synth_panel().search_panel().open(), "search panel should start closed");
    click(controller, panel.searchButton);
    require(controller.synth_panel().search_panel().open(), "search button did not open the search panel");
    {
        const auto& searchLayout = controller.synth_panel().search_panel().layout();
        require(searchLayout.panel.width > 0 && searchLayout.panel.height > 0,
                "search panel was not laid out on open");
        // Clicking the search panel's own close button closes it (overlay consumes the click).
        click(controller, searchLayout.closeButton);
        require(!controller.synth_panel().search_panel().open(),
                "search panel close button did not close the panel");
    }
    click(controller, panel.searchButton);
    require(controller.synth_panel().search_panel().open(), "search button did not reopen the search panel");
    // Audition with no candidates must not crash or change the preset.
    {
        const auto beforeName = controller.synthesizer().preset().name;
        const auto& searchLayout = controller.synth_panel().search_panel().layout();
        click(controller, searchLayout.auditionButton);
        require(controller.synth_panel().search_panel().open(), "audition with no candidates closed the panel");
        require(controller.synthesizer().preset().name == beforeName, "audition with no candidates changed the preset");
    }
}

int main() {
    try {
        using namespace dve::editor;
        auto controllerOwner = std::make_unique<NativeEditorController>(EditorWorkspace(make_native_editor_demo_document()));
        auto& controller = *controllerOwner;
        require(std::as_const(controller.workspace().menus()).find("window.toggle_synth") != nullptr,
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

        // Like the controller, the layout is large; it carries from page to page as before.
        auto panelOwner = std::make_unique<SynthPanelLayout>();
        auto& panel = *panelOwner;
        oscillator_and_filter_pages(controller, panel);
        wavetable_section(controller, panel);
        oscillator_sources(controller, panel);
        modulation_and_performance_pages(controller, panel);
        effects_page(controller, panel);
        expression_page(controller, panel);
        presets_and_search(controller, panel);

        require(controller.dispatch_action("window.toggle_synth"), "synth close action failed");
        require(!controller.synth_panel().open(), "synth panel did not close");
        require(!controller.synth_panel().search_panel().open(),
                "search panel stayed open after the synth panel closed");
        std::cout << "dve_editor_synth_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_synth_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
