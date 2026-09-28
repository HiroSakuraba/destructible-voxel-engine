// Tests for the four arpeggiator upgrades: humanize, dotted divisions,
// arp-level scale lock, and Elektron-style A:B conditional trigs.
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

dve::audio::SynthPreset focused_preset() {
    dve::audio::SynthPreset preset = dve::audio::SynthPreset::make_default();
    preset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Sine;
    preset.oscillators[0].gain = 0.5F;
    for (std::size_t i = 1; i < preset.oscillators.size(); ++i) preset.oscillators[i].gain = 0.0F;
    preset.filter.enabled = false;
    preset.distortion.enabled = false; preset.chorus.enabled = false;
    preset.delay.enabled = false; preset.reverb.enabled = false;
    preset.compressor.enabled = false; preset.limiter.enabled = false;
    preset.eq.enabled = false;
    return preset;
}

std::vector<dve::audio::MidiMessage> collect_note_ons(dve::audio::Synthesizer& synth) {
    std::vector<dve::audio::MidiMessage> noteOns;
    dve::audio::MidiMessage output;
    while (synth.poll_midi_output(output))
        if (output.is_note_on()) noteOns.push_back(output);
    return noteOns;
}

void test_dotted_divisions() {
    using namespace dve::audio;
    // Dotted 8th at 120 BPM = 0.75 beats = 0.375 s = 18000 frames @ 48 kHz.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.mode = ArpeggiatorMode::Up;
        preset.arpeggiator.division = ArpeggiatorDivision::DottedEighth;
        preset.arpeggiator.tempoBpm = 120.0F;
        preset.arpeggiator.gate = 0.5F;
        preset.arpeggiator.swing = 0.0F;
        preset.arpeggiator.octaveRange = 1;
        preset.arpeggiator.stepCount = 1;
        preset.arpeggiator.steps[0] = {};
        preset.arpeggiator.sendMidiOutput = true;
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F) && synth.note_on(64, 0.8F), "held-note input failed");
        std::vector<float> audio(40000U * 2U);
        synth.render(audio);
        auto noteOns = collect_note_ons(synth);
        require(noteOns.size() >= 2U, "dotted-8th arp produced too few notes");
        require(noteOns[0].sampleFrame == 0U, "dotted-8th first step was not at frame 0");
        // 18000 frames, allow small tolerance for block processing
        const auto gap = noteOns[1].sampleFrame - noteOns[0].sampleFrame;
        require(gap >= 17500U && gap <= 18500U, "dotted-8th step was not ~18000 frames");
    }
    // 64th at 120 BPM = 0.0625 beats = 0.03125 s = 1500 frames @ 48 kHz.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.mode = ArpeggiatorMode::Up;
        preset.arpeggiator.division = ArpeggiatorDivision::SixtyFourth;
        preset.arpeggiator.tempoBpm = 120.0F;
        preset.arpeggiator.gate = 0.5F;
        preset.arpeggiator.swing = 0.0F;
        preset.arpeggiator.octaveRange = 1;
        preset.arpeggiator.stepCount = 1;
        preset.arpeggiator.steps[0] = {};
        preset.arpeggiator.sendMidiOutput = true;
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F) && synth.note_on(64, 0.8F), "held-note input failed");
        std::vector<float> audio(10000U * 2U);
        synth.render(audio);
        auto noteOns = collect_note_ons(synth);
        require(noteOns.size() >= 2U, "64th arp produced too few notes");
        const auto gap = noteOns[1].sampleFrame - noteOns[0].sampleFrame;
        require(gap >= 1400U && gap <= 1600U, "64th step was not ~1500 frames");
    }
    std::cout << "dotted divisions: OK\n";
}

void test_arp_scale_lock() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    auto preset = focused_preset();
    preset.arpeggiator.enabled = true;
    preset.arpeggiator.mode = ArpeggiatorMode::Up;
    preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
    preset.arpeggiator.tempoBpm = 120.0F;
    preset.arpeggiator.gate = 0.5F;
    preset.arpeggiator.swing = 0.0F;
    preset.arpeggiator.octaveRange = 1;
    preset.arpeggiator.stepCount = 1;
    preset.arpeggiator.steps[0] = {};
    preset.arpeggiator.steps[0].transpose = 1; // C -> C#, out of C major
    preset.arpeggiator.scale = ChordScale::Major;
    preset.arpeggiator.scaleRoot = 0; // C
    preset.arpeggiator.sendMidiOutput = true;
    synth.set_preset(preset);
    require(synth.note_on(60, 0.8F), "held-note input failed");
    std::vector<float> audio(13000U * 2U);
    synth.render(audio);
    auto noteOns = collect_note_ons(synth);
    require(!noteOns.empty(), "scale-lock arp produced no notes");
    // 61 (C#) is not in C major; quantize should snap to 60 or 62, never 61.
    for (const auto& m : noteOns) require(m.data1 != 61U, "scale lock failed to quantize C#");
    std::cout << "arp scale lock: OK (C# -> " << static_cast<int>(noteOns[0].data1) << ")\n";
}

void test_ab_condition() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    auto preset = focused_preset();
    preset.arpeggiator.enabled = true;
    preset.arpeggiator.mode = ArpeggiatorMode::Up;
    preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
    preset.arpeggiator.tempoBpm = 120.0F;
    preset.arpeggiator.gate = 0.9F;
    preset.arpeggiator.swing = 0.0F;
    preset.arpeggiator.octaveRange = 1;
    preset.arpeggiator.stepCount = 1;
    preset.arpeggiator.steps[0] = {};
    preset.arpeggiator.steps[0].condition = ArpeggiatorCondition::AB;
    preset.arpeggiator.steps[0].conditionA = 2;
    preset.arpeggiator.steps[0].conditionB = 3;
    preset.arpeggiator.sendMidiOutput = true;
    synth.set_preset(preset);
    require(synth.note_on(60, 0.8F), "held-note input failed");
    // 16th at 120 BPM = 6000 frames. Render 8 steps (48000 frames).
    std::vector<float> audio(50000U * 2U);
    synth.render(audio);
    auto noteOns = collect_note_ons(synth);
    // A=2,B=3 fires on loop passes 1,4,7... (0-indexed). With stepCount=1,
    // step N is loop pass N. So notes at steps 1,4,7 -> frames ~6000, 24000, 42000.
    // Step 0 (frame 0) must NOT fire.
    require(!noteOns.empty(), "A:B arp produced no notes");
    for (const auto& m : noteOns) require(m.sampleFrame != 0U, "A:B fired on pass 0 (should be pass 1)");
    // First note should be near frame 6000 (step 1).
    require(noteOns[0].sampleFrame >= 5500U && noteOns[0].sampleFrame <= 6500U,
            "A:B first note was not at step 1");
    std::cout << "A:B condition: OK (" << noteOns.size() << " notes, first at "
              << noteOns[0].sampleFrame << ")\n";
}

void test_humanize() {
    using namespace dve::audio;
    // Velocity humanize: with amount=1, velocities across steps should vary.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.mode = ArpeggiatorMode::Up;
        preset.arpeggiator.division = ArpeggiatorDivision::ThirtySecond;
        preset.arpeggiator.tempoBpm = 120.0F;
        preset.arpeggiator.gate = 0.5F;
        preset.arpeggiator.swing = 0.0F;
        preset.arpeggiator.octaveRange = 1;
        preset.arpeggiator.stepCount = 1;
        preset.arpeggiator.steps[0] = {};
        preset.arpeggiator.humanizeVelocity = 1.0F;
        preset.arpeggiator.sendMidiOutput = true;
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F), "held-note input failed");
        std::vector<float> audio(60000U * 2U);
        synth.render(audio);
        auto noteOns = collect_note_ons(synth);
        require(noteOns.size() >= 8U, "humanize test produced too few notes");
        std::set<unsigned> velocities;
        for (const auto& m : noteOns) velocities.insert(m.data2);
        require(velocities.size() > 1U, "humanize velocity did not vary velocities");
    }
    // Timing humanize: with amount=1, step intervals should vary from the grid.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.mode = ArpeggiatorMode::Up;
        preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
        preset.arpeggiator.tempoBpm = 120.0F;
        preset.arpeggiator.gate = 0.5F;
        preset.arpeggiator.swing = 0.0F;
        preset.arpeggiator.octaveRange = 1;
        preset.arpeggiator.stepCount = 1;
        preset.arpeggiator.steps[0] = {};
        preset.arpeggiator.humanizeTiming = 1.0F;
        preset.arpeggiator.sendMidiOutput = true;
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F) && synth.note_on(64, 0.8F), "held-note input failed");
        std::vector<float> audio(100000U * 2U);
        synth.render(audio);
        auto noteOns = collect_note_ons(synth);
        require(noteOns.size() >= 8U, "humanize timing test produced too few notes");
        std::set<std::uint64_t> gaps;
        for (std::size_t i = 1; i < noteOns.size(); ++i)
            gaps.insert(noteOns[i].sampleFrame - noteOns[i-1].sampleFrame);
        require(gaps.size() > 1U, "humanize timing did not vary step intervals");
    }
    std::cout << "humanize: OK\n";
}

void test_arp_serialize_roundtrip() {
    using namespace dve::audio;
    Synthesizer synth(48000);
    auto preset = focused_preset();
    preset.arpeggiator.humanizeTiming = 0.42F;
    preset.arpeggiator.humanizeVelocity = 0.77F;
    preset.arpeggiator.scale = ChordScale::Dorian;
    preset.arpeggiator.scaleRoot = 7;
    preset.arpeggiator.division = ArpeggiatorDivision::DottedQuarter;
    preset.arpeggiator.steps[3].condition = ArpeggiatorCondition::AB;
    preset.arpeggiator.steps[3].conditionA = 3;
    preset.arpeggiator.steps[3].conditionB = 4;
    require(preset.validate(), "validate failed on new arp fields");
    const auto text = preset.serialize();
    const auto parsed = dve::audio::SynthPreset::parse(text);
    require(parsed.has_value(), "parse failed on new arp fields");
    const auto& a = parsed->arpeggiator;
    require(std::fabs(a.humanizeTiming - 0.42F) < 1e-4F, "humanizeTiming round-trip failed");
    require(std::fabs(a.humanizeVelocity - 0.77F) < 1e-4F, "humanizeVelocity round-trip failed");
    require(a.scale == ChordScale::Dorian, "arp scale round-trip failed");
    require(a.scaleRoot == 7U, "arp scaleRoot round-trip failed");
    require(a.division == ArpeggiatorDivision::DottedQuarter, "dotted division round-trip failed");
    require(a.steps[3].condition == ArpeggiatorCondition::AB, "AB condition round-trip failed");
    require(a.steps[3].conditionA == 3U && a.steps[3].conditionB == 4U, "A:B params round-trip failed");
    std::cout << "arp serialize round-trip: OK\n";
}

} // namespace

int main() {
    try {
        test_dotted_divisions();
        test_arp_scale_lock();
        test_ab_condition();
        test_humanize();
        test_arp_serialize_roundtrip();
        std::cout << "ALL ARP TESTS PASSED\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
