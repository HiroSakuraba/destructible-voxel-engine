// Tests for the Phase 3 (SYN-012) generative step sequencer.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dve/audio/sequencer.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr float kSampleRate = 48000.0F;
constexpr float kTempo = 120.0F;
// One 16th-note step at 120 BPM / 48 kHz.
constexpr std::uint32_t kStepFrames = 6000;

struct CapturedEvent {
    bool noteOn;
    std::uint8_t note;
    float velocity;
    std::uint32_t frameOffset;
};

struct EventLog {
    std::vector<CapturedEvent> events;
    Sequencer::EventCallback callback() {
        return [this](const Sequencer::Event& e) {
            events.push_back({e.noteOn, e.note, e.velocity, e.frameOffset});
        };
    }
    std::size_t note_ons() const {
        std::size_t n = 0;
        for (const auto& e : events) n += e.noteOn ? 1 : 0;
        return n;
    }
    std::size_t note_offs() const {
        std::size_t n = 0;
        for (const auto& e : events) n += e.noteOn ? 0 : 1;
        return n;
    }
    void clear() { events.clear(); }
};

// Configures a plain 4-step pitch lane (chromatic, root 60) with full gate.
void configure_basic_pattern(Sequencer& seq) {
    seq.set_enabled(true);
    seq.set_scale(SequencerScale::Chromatic);
    seq.set_root_note(60);
    seq.set_octave_range(0);
    seq.set_lane_step_count(SequencerLane::Pitch, 4);
    for (std::uint8_t i = 0; i < 4; ++i) {
        SequencerStep step;
        step.value = static_cast<float>(i);  // notes 60..63
        seq.set_lane_step(SequencerLane::Pitch, i, step);
    }
    seq.set_lane_step_count(SequencerLane::Velocity, 1);
    SequencerStep vel;
    vel.value = 0.9F;
    seq.set_lane_step(SequencerLane::Velocity, 0, vel);
    seq.set_lane_step_count(SequencerLane::Gate, 1);
    SequencerStep gate;
    gate.value = 1.0F;
    seq.set_lane_step(SequencerLane::Gate, 0, gate);
}

}  // namespace

int main() {
    // Test 1: polymeter independence — pitch lane 7 steps, velocity lane 5
    // steps realign after lcm(7,5) = 35 steps.
    {
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.set_lane_step_count(SequencerLane::Pitch, 7);
        seq.set_lane_step_count(SequencerLane::Velocity, 5);
        seq.start();
        EventLog log;
        std::vector<std::pair<std::uint8_t, std::uint8_t>> fired;
        for (int tick = 0; tick < 36; ++tick) {
            fired.emplace_back(seq.current_step(SequencerLane::Pitch),
                               seq.current_step(SequencerLane::Velocity));
            seq.process(kStepFrames, kSampleRate, kTempo, log.callback());
        }
        CHECK(fired[0].first == 0 && fired[0].second == 0);
        CHECK(fired[1].first == 1 && fired[1].second == 1);
        CHECK(fired[34].first == 6 && fired[34].second == 4);
        // After 35 steps both lanes are back at 0: realigned.
        CHECK(fired[35].first == 0 && fired[35].second == 0);
        std::printf("polymeter: 36 ticks -> %zu note-ons\n", log.note_ons());
        CHECK(log.note_ons() == 36);
    }

    // Test 2: probability gating — p=0 never triggers; probability lane at 0
    // also mutes.
    {
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.set_lane_step_count(SequencerLane::Pitch, 2);
        SequencerStep muted;
        muted.value = 0.0F;
        muted.probability = 0.0F;
        seq.set_lane_step(SequencerLane::Pitch, 0, muted);
        seq.set_lane_step(SequencerLane::Pitch, 1, muted);
        seq.start();
        EventLog log;
        for (int tick = 0; tick < 200; ++tick)
            seq.process(kStepFrames, kSampleRate, kTempo, log.callback());
        std::printf("probability p=0: %zu note-ons over 200 ticks\n", log.note_ons());
        CHECK(log.note_ons() == 0);

        // Probability lane value 0 mutes even with per-step p=1.
        Sequencer seq2;
        configure_basic_pattern(seq2);
        seq2.set_lane_step_count(SequencerLane::Probability, 1);
        SequencerStep pstep;
        pstep.value = 0.0F;
        seq2.set_lane_step(SequencerLane::Probability, 0, pstep);
        seq2.start();
        EventLog log2;
        for (int tick = 0; tick < 20; ++tick)
            seq2.process(kStepFrames, kSampleRate, kTempo, log2.callback());
        CHECK(log2.note_ons() == 0);

        // p=1 fires every step.
        Sequencer seq3;
        configure_basic_pattern(seq3);
        seq3.start();
        EventLog log3;
        for (int tick = 0; tick < 8; ++tick)
            seq3.process(kStepFrames, kSampleRate, kTempo, log3.callback());
        CHECK(log3.note_ons() == 8);
    }

    // Test 3: ratchets subdivide a step into N triggers.
    {
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.set_lane_step_count(SequencerLane::Pitch, 1);
        SequencerStep ratcheted;
        ratcheted.value = 0.0F;
        ratcheted.ratchets = 4;
        seq.set_lane_step(SequencerLane::Pitch, 0, ratcheted);
        seq.start();
        EventLog log;
        seq.process(kStepFrames, kSampleRate, kTempo, log.callback());
        std::printf("ratchets=4: %zu ons, %zu offs\n", log.note_ons(), log.note_offs());
        CHECK(log.note_ons() == 4);
        CHECK(log.note_offs() == 4);
        std::vector<std::uint32_t> onsets;
        for (const auto& e : log.events)
            if (e.noteOn) onsets.push_back(e.frameOffset);
        CHECK(onsets.size() == 4);
        CHECK(onsets[0] == 0);
        CHECK(onsets[1] == kStepFrames / 4);
        CHECK(onsets[2] == kStepFrames / 2);
        CHECK(onsets[3] == 3 * kStepFrames / 4);
    }

    // Test 4: conditionals — EveryNth fires every Nth pitch-lane cycle.
    {
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.set_lane_step_count(SequencerLane::Pitch, 1);
        SequencerStep cond;
        cond.value = 0.0F;
        cond.condition = SequencerStepCondition::EveryNth;
        cond.conditionN = 2;
        seq.set_lane_step(SequencerLane::Pitch, 0, cond);
        seq.start();
        EventLog log;
        for (int tick = 0; tick < 6; ++tick)
            seq.process(kStepFrames, kSampleRate, kTempo, log.callback());
        std::printf("every-2nd-cycle: %zu note-ons over 6 ticks\n", log.note_ons());
        CHECK(log.note_ons() == 3);  // cycles 0, 2, 4

        // FirstCycleOnly.
        Sequencer seq2;
        configure_basic_pattern(seq2);
        seq2.set_lane_step_count(SequencerLane::Pitch, 1);
        SequencerStep first;
        first.value = 0.0F;
        first.condition = SequencerStepCondition::FirstCycleOnly;
        seq2.set_lane_step(SequencerLane::Pitch, 0, first);
        seq2.start();
        EventLog log2;
        for (int tick = 0; tick < 4; ++tick)
            seq2.process(kStepFrames, kSampleRate, kTempo, log2.callback());
        CHECK(log2.note_ons() == 1);

        // LastCycleOnly with patternCycles=4: fires on cycles 3 and 7.
        Sequencer seq3;
        configure_basic_pattern(seq3);
        seq3.set_lane_step_count(SequencerLane::Pitch, 1);
        SequencerStep last;
        last.value = 0.0F;
        last.condition = SequencerStepCondition::LastCycleOnly;
        seq3.set_lane_step(SequencerLane::Pitch, 0, last);
        seq3.lane(SequencerLane::Pitch).patternCycles = 4;
        seq3.start();
        EventLog log3;
        for (int tick = 0; tick < 8; ++tick)
            seq3.process(kStepFrames, kSampleRate, kTempo, log3.callback());
        CHECK(log3.note_ons() == 2);
    }

    // Test 5: scale quantization snaps pitch offsets to scale degrees.
    {
        CHECK(Sequencer::quantize_offset(SequencerScale::Major, 0) == 0);
        CHECK(Sequencer::quantize_offset(SequencerScale::Major, 4) == 4);   // E is in C major
        CHECK(Sequencer::quantize_offset(SequencerScale::Major, 1) == 2);   // C# -> D (tie up)
        CHECK(Sequencer::quantize_offset(SequencerScale::Minor, 6) == 7);   // F#: tie 5/7 -> up
        CHECK(Sequencer::quantize_offset(SequencerScale::PentatonicMinor, 4) == 5);  // tie -> 5
        CHECK(Sequencer::quantize_offset(SequencerScale::Chromatic, 11) == 11);
        CHECK(Sequencer::quantize_offset(SequencerScale::Dorian, 10) == 10);
        // Full pipeline: major scale, root C4.
        CHECK(Sequencer::note_for_offset(SequencerScale::Major, 60, 2, 1) == 62);
        CHECK(Sequencer::note_for_offset(SequencerScale::Major, 60, 2, 7) == 67);
        // Octave wrap into [root, root + range*12].
        CHECK(Sequencer::note_for_offset(SequencerScale::Chromatic, 60, 1, 20) == 68);
        // No wrap when octaveRange == 0.
        CHECK(Sequencer::note_for_offset(SequencerScale::Major, 60, 0, 25) == 86);
        // End-to-end: a C# pitch step in C major plays D.
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.set_scale(SequencerScale::Major);
        seq.set_root_note(60);
        seq.set_octave_range(2);
        seq.set_lane_step_count(SequencerLane::Pitch, 1);
        SequencerStep sharp;
        sharp.value = 1.0F;
        seq.set_lane_step(SequencerLane::Pitch, 0, sharp);
        seq.start();
        EventLog log;
        seq.process(kStepFrames, kSampleRate, kTempo, log.callback());
        CHECK(log.events.size() >= 1 && log.events[0].noteOn);
        CHECK(log.events[0].note == 62);
    }

    // Test 6: tempo math — steps are 16th notes: 60/bpm/4 seconds.
    {
        CHECK(std::abs(Sequencer::step_duration_seconds(120.0) - 0.125) < 1e-9);
        CHECK(std::abs(Sequencer::step_duration_seconds(60.0) - 0.25) < 1e-9);
        CHECK(std::abs(Sequencer::step_duration_seconds(140.0) - (60.0 / 140.0 / 4.0)) < 1e-9);
        // At 120 BPM / 48 kHz a step is 6000 frames: two process calls of two
        // steps' worth of frames produce onsets at 0 and 6000.
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.start();
        EventLog log;
        seq.process(2 * kStepFrames, kSampleRate, kTempo, log.callback());
        std::vector<std::uint32_t> onsets;
        for (const auto& e : log.events)
            if (e.noteOn) onsets.push_back(e.frameOffset);
        CHECK(onsets.size() == 2);
        CHECK(onsets[0] == 0);
        CHECK(onsets[1] == kStepFrames);
    }

    // Test 7: morph/timbre/pan lane currents are readable and appear as
    // modulation sources (tokens appended after Lorenz; round-trip parse).
    {
        CHECK(static_cast<int>(ModulationSource::SeqTimbre) ==
              static_cast<int>(ModulationSource::Lorenz) + 1);
        CHECK(static_cast<int>(ModulationSource::SeqMorph) ==
              static_cast<int>(ModulationSource::Lorenz) + 2);
        CHECK(static_cast<int>(ModulationSource::SeqPan) ==
              static_cast<int>(ModulationSource::Lorenz) + 3);
        CHECK(modulation_source_name(ModulationSource::SeqTimbre) == "seq_timbre");
        CHECK(modulation_source_name(ModulationSource::SeqMorph) == "seq_morph");
        CHECK(modulation_source_name(ModulationSource::SeqPan) == "seq_pan");
        // Existing tokens are untouched (serialization stability).
        CHECK(modulation_source_name(ModulationSource::Lorenz) == "lorenz");
        CHECK(modulation_source_name(ModulationSource::Off) == "none");

        Sequencer seq;
        seq.set_enabled(true);
        seq.set_lane_step_count(SequencerLane::Timbre, 1);
        seq.set_lane_step_count(SequencerLane::Morph, 1);
        seq.set_lane_step_count(SequencerLane::Pan, 1);
        SequencerStep t, m, p;
        t.value = 0.75F; m.value = 0.25F; p.value = -0.5F;
        seq.set_lane_step(SequencerLane::Timbre, 0, t);
        seq.set_lane_step(SequencerLane::Morph, 0, m);
        seq.set_lane_step(SequencerLane::Pan, 0, p);
        seq.start();
        CHECK(std::abs(seq.timbre_value() - 0.75F) < 1e-6F);
        CHECK(std::abs(seq.morph_value() - 0.25F) < 1e-6F);
        CHECK(std::abs(seq.pan_value() + 0.5F) < 1e-6F);

        // End-to-end through the synth's modulation matrix: route SeqTimbre
        // to VoiceGain with no smoothing; the telemetry must read the lane value.
        Synthesizer synth(static_cast<std::uint32_t>(kSampleRate));
        SynthPreset preset = SynthPreset::make_default();
        preset.modulation[0] = {true, ModulationSource::SeqTimbre,
                                ModulationDestination::VoiceGain, 1.0F, 0.0F,
                                ModulationCurve::Linear, ModulationPolarity::Bipolar, 0.0F};
        synth.set_preset(preset);
        Sequencer& sseq = synth.sequencer();
        sseq.set_enabled(true);
        sseq.set_lane_step_count(SequencerLane::Timbre, 1);
        SequencerStep ts;
        ts.value = 0.75F;
        sseq.set_lane_step(SequencerLane::Timbre, 0, ts);
        sseq.start();
        synth.note_on(60, 0.9F, 0);
        std::vector<float> buf(9600 * 2, 0.0F);
        synth.render(buf.data(), 9600);
        const auto activity = synth.modulation_activity();
        std::printf("seq_timbre mod telemetry: %.4f\n", activity[0].currentValue);
        CHECK(activity[0].enabled);
        CHECK(activity[0].source == ModulationSource::SeqTimbre);
        CHECK(std::abs(activity[0].currentValue - 0.75F) < 0.05F);
        synth.note_off(60, 0.0F, 0);
    }

    // Test 8: seeded step mutation is reproducible.
    {
        auto build = []() {
            Sequencer seq;
            seq.set_random_seed(1234);
            seq.set_lane_step_count(SequencerLane::Pitch, 4);
            seq.lane(SequencerLane::Pitch).mutationAmount = 1.0F;
            for (std::uint8_t i = 0; i < 4; ++i) {
                SequencerStep s;
                s.value = static_cast<float>(i * 2);
                seq.set_lane_step(SequencerLane::Pitch, i, s);
            }
            seq.set_enabled(true);
            seq.start();
            return seq;
        };
        Sequencer a = build();
        Sequencer b = build();
        EventLog log;
        for (int tick = 0; tick < 12; ++tick) {  // 3 full cycles
            a.process(kStepFrames, kSampleRate, kTempo, log.callback());
            b.process(kStepFrames, kSampleRate, kTempo, log.callback());
        }
        CHECK(a.cycle_count(SequencerLane::Pitch) == 3);
        bool identical = true;
        bool mutated = false;
        for (std::uint8_t i = 0; i < 4; ++i) {
            const float va = a.lane(SequencerLane::Pitch).steps[i].value;
            const float vb = b.lane(SequencerLane::Pitch).steps[i].value;
            if (va != vb) identical = false;
            if (std::abs(va - static_cast<float>(i * 2)) > 1e-6F) mutated = true;
        }
        std::printf("mutation reproducible: identical=%d mutated=%d\n", identical, mutated);
        CHECK(identical);
        CHECK(mutated);  // mutationAmount=1 actually drifted the values
        // Zero mutation leaves values untouched.
        Sequencer c;
        c.set_random_seed(999);
        c.set_lane_step_count(SequencerLane::Pitch, 2);
        c.lane(SequencerLane::Pitch).mutationAmount = 0.0F;
        c.set_enabled(true);
        c.start();
        for (int tick = 0; tick < 6; ++tick)
            c.process(kStepFrames, kSampleRate, kTempo, log.callback());
        CHECK(c.lane(SequencerLane::Pitch).steps[0].value == 0.0F);
        CHECK(c.lane(SequencerLane::Pitch).steps[1].value == 0.0F);
    }

    // Test 9: a standalone Sequencer drives Synthesizer via the public
    // note_on/note_off API.
    {
        Synthesizer synth(static_cast<std::uint32_t>(kSampleRate));
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.start();
        bool seenActive = false;
        for (int block = 0; block < 10; ++block) {
            seq.process(3000, kSampleRate, kTempo, [&](const Sequencer::Event& e) {
                if (e.noteOn) synth.note_on(e.note, e.velocity, e.channel);
                else synth.note_off(e.note, 0.0F, e.channel);
            });
            std::vector<float> buf(3000 * 2, 0.0F);
            synth.render(buf.data(), 3000);
            for (const auto& v : synth.voices())
                if (v.active) seenActive = true;
        }
        CHECK(seenActive);
        std::printf("standalone sequencer -> note_on/off: voices went active\n");
    }

    // Test 10: the synth's own sequencer advances once per render block and
    // triggers voices through the render path.
    {
        Synthesizer synth(static_cast<std::uint32_t>(kSampleRate));
        Sequencer& seq = synth.sequencer();
        configure_basic_pattern(seq);
        seq.start();
        std::vector<float> buf(3 * kStepFrames * 2, 0.0F);
        synth.render(buf.data(), kStepFrames);  // exactly one step
        bool anyActive = false;
        for (const auto& v : synth.voices())
            if (v.active) anyActive = true;
        CHECK(anyActive);
        CHECK(seq.cycle_count(SequencerLane::Pitch) == 0);
        // Render 3 more steps -> one full cycle of the 4-step pitch lane.
        synth.render(buf.data(), 3 * kStepFrames);
        CHECK(seq.cycle_count(SequencerLane::Pitch) == 1);
        CHECK(seq.current_step(SequencerLane::Pitch) == 0);
        // Disabled by default on a fresh synth.
        Synthesizer fresh(static_cast<std::uint32_t>(kSampleRate));
        CHECK(!fresh.sequencer().enabled());
    }

    // Test 11: glide defers the note-off to the next step onset (legato).
    {
        Sequencer seq;
        configure_basic_pattern(seq);
        seq.set_lane_step_count(SequencerLane::Pitch, 2);
        SequencerStep g0, g1;
        g0.value = 0.0F; g0.glide = true;
        g1.value = 2.0F; g1.glide = false;
        seq.set_lane_step(SequencerLane::Pitch, 0, g0);
        seq.set_lane_step(SequencerLane::Pitch, 1, g1);
        seq.start();
        EventLog log;
        // One block covering two steps: step 0 (glide) fires at 0, step 1 at kStepFrames.
        seq.process(2 * kStepFrames, kSampleRate, kTempo, log.callback());
        // Step 0's note-off must land exactly on step 1's onset (no gap).
        std::uint32_t off0 = 0, on1 = 0;
        bool foundOff0 = false, foundOn1 = false;
        for (const auto& e : log.events) {
            if (!e.noteOn && e.note == 60) { off0 = e.frameOffset; foundOff0 = true; }
            if (e.noteOn && e.note == 62) { on1 = e.frameOffset; foundOn1 = true; }
        }
        std::printf("glide: off0=%u on1=%u\n", off0, on1);
        CHECK(foundOff0 && foundOn1);
        CHECK(on1 == kStepFrames);
        CHECK(off0 == on1);
    }

    if (g_failures == 0) std::printf("sequencer tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
