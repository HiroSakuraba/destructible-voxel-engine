// Tests for the synth-wide profiler counters (SynthProfiler).
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

dve::audio::SynthPreset quiet_preset() {
    dve::audio::SynthPreset preset = dve::audio::SynthPreset::make_default();
    preset.distortion.enabled = false;
    preset.bitcrusher.enabled = false;
    preset.chorus.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    preset.eq.enabled = false;
    return preset;
}

void test_basic_counters() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    synth.set_preset(quiet_preset());
    synth.reset_profiler();

    auto before = synth.profiler();
    require(before.renderCalls == 0U, "renderCalls not zero after reset");
    require(before.samplesRendered == 0U, "samplesRendered not zero after reset");
    require(before.activeVoices == 0U, "activeVoices not zero after reset");

    // Two render calls of known sizes.
    std::vector<float> audio(1000U * 2U);
    synth.render(audio);
    std::vector<float> audio2(500U * 2U);
    synth.render(audio2);

    auto after = synth.profiler();
    require(after.renderCalls == 2U, "renderCalls did not count 2 renders");
    require(after.samplesRendered == 1500U, "samplesRendered did not match buffer sizes");
    require(after.activeVoices == 0U, "activeVoices should be 0 with no notes");
    // No voices active: no oscillator/filter/FX work counted.
    require(after.oscillatorVoiceFrames == 0U, "oscillatorVoiceFrames should be 0 with no voices");
    std::cout << "basic counters: OK\n";
}

void test_voice_counters() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    auto preset = quiet_preset();
    preset.filter.enabled = true;
    synth.set_preset(preset);
    synth.reset_profiler();

    require(synth.note_on(60, 0.9F), "note_on failed");
    require(synth.note_on(64, 0.9F), "note_on failed");
    require(synth.note_on(67, 0.9F), "note_on failed");
    std::vector<float> audio(4800U * 2U);  // 0.1 s @ 48 kHz
    synth.render(audio);

    auto prof = synth.profiler();
    require(prof.voicesStarted == 3U, "voicesStarted did not count 3 note-ons");
    require(prof.activeVoices == 3U, "activeVoices did not reflect 3 held notes");
    require(prof.maximumActiveVoices >= 3U, "high-water mark missed 3 voices");
    require(prof.oscillatorVoiceFrames == 3U * 4800U, "oscillatorVoiceFrames mismatch");
    require(prof.filterFrames == 4800U, "filterFrames should equal frames with filter on");
    require(prof.fxFrames == 0U, "fxFrames should be 0 with all FX off");
    std::cout << "voice counters: OK\n";
}

void test_voice_stealing() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    synth.set_preset(quiet_preset());
    synth.reset_profiler();

    // kSynthVoiceCount is 16; start 20 notes to force steals.
    for (int n = 0; n < 20; ++n) require(synth.note_on(static_cast<std::uint8_t>(40 + n), 0.9F), "note_on failed");
    std::vector<float> audio(480U * 2U);
    synth.render(audio);

    auto prof = synth.profiler();
    require(prof.voicesStarted == 20U, "voicesStarted did not count 20 note-ons");
    require(prof.voicesStolen == 4U, "voicesStolen did not count 4 steals");
    require(prof.activeVoices == 16U, "activeVoices should cap at 16");
    require(prof.maximumActiveVoices == 16U, "high-water mark should be 16");
    std::cout << "voice stealing: OK\n";
}

void test_fx_and_arp_counters() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    auto preset = quiet_preset();
    preset.delay.enabled = true;
    preset.arpeggiator.enabled = true;
    preset.arpeggiator.mode = ArpeggiatorMode::Up;
    preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
    preset.arpeggiator.tempoBpm = 120.0F;
    preset.arpeggiator.stepCount = 4;
    preset.arpeggiator.steps[0] = {};
    preset.arpeggiator.steps[1] = {};
    preset.arpeggiator.steps[2] = {};
    preset.arpeggiator.steps[3] = {};
    synth.set_preset(preset);
    synth.reset_profiler();

    require(synth.note_on(60, 0.9F), "note_on failed");
    std::vector<float> audio(96000U * 2U);  // 2 s @ 48 kHz
    synth.render(audio);

    auto prof = synth.profiler();
    require(prof.fxFrames == 96000U, "fxFrames should equal frames with delay on");
    require(prof.arpSteps > 0U, "arpSteps did not count arpeggiator steps");
    // 16th at 120 BPM = 0.125 s per step; 2 s should give ~16 steps.
    require(prof.arpSteps >= 10U && prof.arpSteps <= 20U, "arpSteps out of expected range");
    std::cout << "fx and arp counters: OK\n";
}

void test_reset() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    synth.set_preset(quiet_preset());
    require(synth.note_on(60, 0.9F), "note_on failed");
    std::vector<float> audio(480U * 2U);
    synth.render(audio);
    auto before = synth.profiler();
    require(before.renderCalls > 0U, "expected non-zero counters before reset");

    synth.reset_profiler();
    auto after = synth.profiler();
    require(after.renderCalls == 0U, "reset did not clear renderCalls");
    require(after.samplesRendered == 0U, "reset did not clear samplesRendered");
    require(after.voicesStarted == 0U, "reset did not clear voicesStarted");
    require(after.voicesStolen == 0U, "reset did not clear voicesStolen");
    require(after.oscillatorVoiceFrames == 0U, "reset did not clear oscillatorVoiceFrames");
    require(after.arpSteps == 0U, "reset did not clear arpSteps");
    // High-water mark resets to the current live count.
    require(after.maximumActiveVoices == after.activeVoices, "reset did not rebase high-water mark");
    std::cout << "reset: OK\n";
}

}  // namespace

int main() {
    try {
        test_basic_counters();
        test_voice_counters();
        test_voice_stealing();
        test_fx_and_arp_counters();
        test_reset();
        std::cout << "ALL PROFILER TESTS PASSED\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
