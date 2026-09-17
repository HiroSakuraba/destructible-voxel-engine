#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <vector>

#include "dve/audio/midi.hpp"
#include "dve/audio/synthesizer.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool near(float a, float b, float epsilon = 1.0e-4F) { return std::abs(a - b) < epsilon; }

void disable_effects(dve::audio::SynthPreset& preset) {
    preset.distortion.enabled = false; preset.eq.enabled = false; preset.chorus.enabled = false;
    preset.phaser.enabled = false; preset.delay.enabled = false; preset.reverb.enabled = false;
    preset.compressor.enabled = false; preset.limiter.enabled = true;
}

dve::audio::SynthPreset focused_preset() {
    using namespace dve::audio;
    SynthPreset preset = SynthPreset::make_default();
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].gain = 0.32F;
    preset.oscillators[0].waveform = OscillatorWaveform::Saw;
    preset.ampEnvelope = {0.001F, 0.005F, 1.0F, 0.03F, EnvelopeCurve::Linear};
    preset.filter.enabled = false;
    preset.masterGain = 0.55F;
    preset.masterPan = 0.0F;
    preset.tuning.analogDriftCents = 0.0F;
    disable_effects(preset);
    return preset;
}

std::vector<float> render_note(const dve::audio::SynthPreset& preset, std::size_t frames = 8192U) {
    dve::audio::Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(60, 0.82F), "note-on queue failed");
    std::vector<float> audio(frames * 2U);
    synth.render(audio);
    require(std::all_of(audio.begin(), audio.end(), [](float value) { return std::isfinite(value); }),
            "render contained non-finite samples");
    return audio;
}

double rms(const std::vector<float>& audio) {
    double energy = 0.0;
    for (float value : audio) energy += static_cast<double>(value) * value;
    return std::sqrt(energy / static_cast<double>(audio.size()));
}

double mean_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size() == b.size(), "comparison size mismatch");
    double total = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) total += std::abs(static_cast<double>(a[i]) - b[i]);
    return total / static_cast<double>(a.size());
}

void test_midi_codec_and_ports() {
    using namespace dve::audio;
    const MidiMessage note = MidiMessage::note_on(2, 64, 101, 1234);
    const auto parsed = MidiMessage::parse(note.raw_bytes(), 1234);
    require(parsed && parsed->is_note_on() && parsed->channel == 2 && parsed->data1 == 64 && parsed->data2 == 101,
            "MIDI note codec failed");
    const MidiMessage bend = MidiMessage::pitch_bend(1, -4096);
    require(bend.data1 == 0 && bend.data2 == 32, "MIDI pitch bend encoding failed");

    VirtualMidiBackend ports;
    std::vector<MidiMessage> received;
    std::string error;
    require(ports.open_input(0, [&](const MidiMessage& message) { received.push_back(message); }, &error), error.c_str());
    require(ports.open_output(0, &error), error.c_str());
    require(ports.inject(note, &error), error.c_str());
    require(ports.send(MidiMessage::note_off(2, 64), &error), error.c_str());
    require(received.size() == 1 && ports.sent_messages().size() == 1, "virtual MIDI full-duplex failed");
}

void test_waveforms_pwm_and_filters() {
    using namespace dve::audio;
    static constexpr std::array waveforms{
        OscillatorWaveform::Sine, OscillatorWaveform::Saw, OscillatorWaveform::Square,
        OscillatorWaveform::Triangle, OscillatorWaveform::Pulse, OscillatorWaveform::Noise,
        OscillatorWaveform::SuperSaw, OscillatorWaveform::Organ, OscillatorWaveform::FoldedSine,
        OscillatorWaveform::Digital};
    std::array<std::vector<float>, waveforms.size()> renders;
    for (std::size_t i = 0; i < waveforms.size(); ++i) {
        auto preset = focused_preset();
        preset.oscillators[0].waveform = waveforms[i];
        preset.oscillators[0].shape = 0.73F;
        renders[i] = render_note(preset, 4096U);
        require(rms(renders[i]) > 0.005, "a waveform rendered silence");
        require(!oscillator_waveform_name(waveforms[i]).empty(), "waveform display name missing");
    }
    require(mean_abs_difference(renders[0], renders[6]) > 0.005, "sine and supersaw were not distinct");
    require(mean_abs_difference(renders[7], renders[9]) > 0.005, "organ and digital were not distinct");

    auto fixedPulse = focused_preset();
    fixedPulse.oscillators[0].waveform = OscillatorWaveform::Pulse;
    fixedPulse.oscillators[0].pulseWidth = 0.34F;
    fixedPulse.oscillators[0].pwmDepth = 0.0F;
    auto modulatedPulse = fixedPulse;
    modulatedPulse.oscillators[0].pwmDepth = 0.85F;
    modulatedPulse.oscillators[0].pwmRateHertz = 7.0F;
    require(mean_abs_difference(render_note(fixedPulse), render_note(modulatedPulse)) > 0.002,
            "pulse-width modulation did not alter the signal");

    static constexpr std::array topologies{
        FilterTopology::CleanStateVariable, FilterTopology::MoogLadder,
        FilterTopology::KorgMs20, FilterTopology::OberheimSem};
    std::array<std::vector<float>, topologies.size()> filtered;
    for (std::size_t i = 0; i < topologies.size(); ++i) {
        auto preset = focused_preset();
        preset.oscillators[0].waveform = OscillatorWaveform::Saw;
        preset.filter.enabled = true;
        preset.filter.topology = topologies[i];
        preset.filter.mode = FilterMode::LowPass;
        preset.filter.cutoffHertz = 1200.0F;
        preset.filter.resonance = 0.68F;
        preset.filter.drive = 2.2F;
        preset.filter.envelopeAmountOctaves = 0.0F;
        preset.filter.keyTrack = 0.0F;
        preset.filter.morph = topologies[i] == FilterTopology::OberheimSem ? 0.28F : 0.0F;
        filtered[i] = render_note(preset);
        require(rms(filtered[i]) > 0.001, "a filter model rendered silence");
        require(!filter_topology_name(topologies[i]).empty(), "filter topology display name missing");
    }
    for (std::size_t i = 0; i < filtered.size(); ++i)
        for (std::size_t j = i + 1; j < filtered.size(); ++j)
            require(mean_abs_difference(filtered[i], filtered[j]) > 0.0001,
                    "filter models were not measurably distinct");
}

void test_chords_and_arpeggiator() {
    using namespace dve::audio;
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.chord.enabled = true;
        preset.chord.type = ChordType::Major7;
        preset.chord.velocityScale = 1.0F;
        preset.arpeggiator.sendMidiOutput = true;
        synth.set_preset(preset);
        require(synth.note_on(60, 1.0F), "chord note-on failed");
        std::array<float, 128> audio{};
        synth.render(audio.data(), audio.size() / 2U);
        std::set<unsigned> notes;
        MidiMessage output;
        while (synth.poll_midi_output(output)) if (output.is_note_on()) notes.insert(output.data1);
        require(notes == std::set<unsigned>({60U, 64U, 67U, 71U}), "major-7 chord voicing was incorrect");
        require(synth.meters().activeVoices == 4U, "chord did not allocate four voices");
    }

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
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F) && synth.note_on(64, 0.8F) && synth.note_on(67, 0.8F),
                "arpeggiator held-note input failed");
        std::vector<float> audio(13000U * 2U);
        synth.render(audio);
        std::vector<MidiMessage> noteOns;
        MidiMessage output;
        while (synth.poll_midi_output(output)) if (output.is_note_on()) noteOns.push_back(output);
        require(noteOns.size() >= 3U, "arpeggiator produced too few notes");
        require(noteOns[0].data1 == 60U && noteOns[0].sampleFrame == 0U,
                "arpeggiator first step was incorrect");
        require(noteOns[1].data1 == 64U && noteOns[1].sampleFrame == 6000U,
                "arpeggiator second step was not sample accurate");
        require(noteOns[2].data1 == 67U && noteOns[2].sampleFrame == 12000U,
                "arpeggiator third step was not sample accurate");
        const auto meter = synth.meters();
        require(meter.heldArpeggiatorNotes == 3U, "arpeggiator held-note telemetry was incorrect");
    }
}


void test_v124_modulation_wavetable_and_filters(const std::filesystem::path& root) {
    using namespace dve::audio;

    // Delay and hold are separate, observable envelope phases.
    {
        auto preset = focused_preset();
        preset.ampEnvelope = {0.001F, 0.002F, 1.0F, 0.03F, EnvelopeCurve::Linear, 0.010F, 0.010F};
        Synthesizer synth(48000);
        synth.set_preset(preset);
        require(synth.note_on(60, 1.0F), "delayed envelope note-on failed");
        std::vector<float> silent(240U * 2U);
        synth.render(silent);
        require(rms(silent) < 1.0e-7, "envelope delay emitted audio");
        require(synth.voices()[0].stage == VoiceStage::Delay, "voice did not enter delay stage");
        std::vector<float> audible(720U * 2U);
        synth.render(audible);
        require(rms(audible) > 0.002, "envelope did not leave delay/hold sequence");
    }

    // Two reusable LFOs and the fixed modulation matrix must alter the signal and remain bounded.
    {
        auto base = focused_preset();
        base.oscillators[0].waveform = OscillatorWaveform::Sine;
        auto modulated = base;
        modulated.lfos[0] = {true, LfoWaveform::Triangle, 5.0F, 1.0F, 0.0F, 0.0F, true, false, 1.0F};
        modulated.modulation[0] = {true, ModulationSource::Lfo1, ModulationDestination::Osc1Pitch,
                                  0.75F, ModulationCurve::Linear};
        require(mean_abs_difference(render_note(base), render_note(modulated)) > 0.003,
                "LFO pitch modulation did not alter the signal");
        for (std::size_t i = 0; i < modulated.modulation.size(); ++i) {
            modulated.modulation[i].enabled = true;
            modulated.modulation[i].source = (i & 1U) == 0U ? ModulationSource::Lfo1 : ModulationSource::Lfo2;
            modulated.modulation[i].destination = ModulationDestination::FilterCutoff;
            modulated.modulation[i].amount = (i & 1U) == 0U ? 1.0F : -1.0F;
            modulated.modulation[i].curve = ModulationCurve::Cubic;
        }
        modulated.lfos[1] = {true, LfoWaveform::SmoothRandom, 17.0F, 1.0F, 0.31F, 0.0F, true, false, 1.0F};
        const auto stress = render_note(modulated, 16384U);
        require(*std::max_element(stress.begin(), stress.end()) <= 1.01F,
                "modulation stress escaped output limiter");
    }

    // MIDI learn controls macros without rebuilding the preset on the callback thread.
    {
        auto preset = focused_preset();
        preset.macros.values[0] = 0.0F;
        preset.modulation[0] = {true, ModulationSource::Macro1, ModulationDestination::VoiceGain,
                                1.0F, ModulationCurve::Linear};
        preset.midiLearn[0] = {true, 74U, 0U, 0.0F, 1.0F, false};
        Synthesizer synth(48000);
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F), "macro test note-on failed");
        std::vector<float> before(2048U * 2U);
        synth.render(before);
        require(synth.control_change(74U, 127U), "MIDI-learn controller post failed");
        std::vector<float> after(2048U * 2U);
        synth.render(after);
        require(rms(after) > rms(before) * 1.35, "MIDI learn did not drive the assigned macro");
    }

    // Sync, FM, ring modulation and a sub oscillator each provide a distinct signal path.
    {
        auto base = focused_preset();
        base.oscillators[0].waveform = OscillatorWaveform::Sine;
        base.oscillators[0].semitones = -12.0F;
        base.oscillators[0].gain = 0.04F;
        base.oscillators[1] = base.oscillators[0];
        base.oscillators[1].waveform = OscillatorWaveform::Saw;
        base.oscillators[1].semitones = 7.0F;
        base.oscillators[1].gain = 0.28F;
        const auto unmodulated = render_note(base);

        auto sync = base;
        sync.oscillators[1].hardSyncSource = 0;
        require(mean_abs_difference(unmodulated, render_note(sync)) > 0.001,
                "hard sync did not alter the target oscillator");

        auto fm = base;
        fm.oscillators[1].frequencyModSource = 0;
        fm.oscillators[1].frequencyModMode = FrequencyModulationMode::Exponential;
        fm.oscillators[1].frequencyModAmount = 0.55F;
        require(mean_abs_difference(unmodulated, render_note(fm)) > 0.003,
                "exponential frequency modulation did not alter the signal");

        auto ring = base;
        ring.oscillators[1].ringModSource = 0;
        ring.oscillators[1].ringModDepth = 0.8F;
        require(mean_abs_difference(unmodulated, render_note(ring)) > 0.003,
                "ring modulation did not alter the signal");

        auto sub = base;
        sub.oscillators[1].subOscillatorLevel = 0.8F;
        sub.oscillators[1].subOscillatorOctaves = 2U;
        require(mean_abs_difference(unmodulated, render_note(sub)) > 0.003,
                "sub oscillator did not alter the signal");
    }

    // Import a multi-frame wavetable through the real audio importer and round-trip it in v3 presets.
    {
        constexpr std::size_t sourceFrames = kWavetableFrameCount * kWavetableSampleCount;
        std::vector<float> source(sourceFrames * 2U);
        for (std::size_t frame = 0; frame < kWavetableFrameCount; ++frame) {
            const float harmonic = 1.0F + static_cast<float>(frame);
            for (std::size_t sample = 0; sample < kWavetableSampleCount; ++sample) {
                const float phase = static_cast<float>(sample) / static_cast<float>(kWavetableSampleCount);
                const float value = 0.65F * std::sin(phase * 6.28318530718F) +
                                    0.30F * std::sin(phase * 6.28318530718F * harmonic);
                const std::size_t index = frame * kWavetableSampleCount + sample;
                source[index * 2U] = value;
                source[index * 2U + 1U] = value;
            }
        }
        std::string error;
        const auto wavPath = root / "wavetable_source.wav";
        require(write_float_wav(wavPath, source, 48000U, &error), error.c_str());
        const auto bank = import_wavetable_from_audio(wavPath, &error);
        require(bank && bank->enabled && bank->frameCount == kWavetableFrameCount && bank->contentHash != 0U,
                "wavetable import failed");
        auto preset = focused_preset();
        preset.wavetable = *bank;
        preset.oscillators[0].waveform = OscillatorWaveform::Wavetable;
        preset.oscillators[0].wavetablePosition = 0.72F;
        preset.oscillators[0].shape = 0.0F;
        const auto rendered = render_note(preset);
        require(rms(rendered) > 0.005, "imported wavetable rendered silence");
        const std::string serialized = preset.serialize();
    require(serialized.starts_with("DVE_SYNTH_PRESET=5\n"), "current preset format did not advance to version 5");
    const auto decoded = SynthPreset::parse(serialized, &error);
        require(decoded && decoded->wavetable.enabled && decoded->wavetable.frameCount == bank->frameCount &&
                near(decoded->wavetable.samples[257], bank->samples[257], 1.0e-5F),
                "version-3 wavetable preset round trip failed");
    }

    // Filter oversampling modes must remain finite and produce measurably different nonlinear responses.
    {
        std::array<std::vector<float>, 3> outputs;
        static constexpr std::array modes{FilterOversampling::X1, FilterOversampling::X2, FilterOversampling::X4};
        for (std::size_t i = 0; i < modes.size(); ++i) {
            auto preset = focused_preset();
            preset.filter.enabled = true;
            preset.filter.topology = FilterTopology::KorgMs20;
            preset.filter.cutoffHertz = 950.0F;
            preset.filter.ms20HighPassCutoffHertz = 90.0F;
            preset.filter.resonance = 0.88F;
            preset.filter.selfOscillation = 1.0F;
            preset.filter.drive = 4.0F;
            preset.filter.oversampling = modes[i];
            outputs[i] = render_note(preset, 4096U);
        }
        require(mean_abs_difference(outputs[0], outputs[1]) > 1.0e-5 &&
                mean_abs_difference(outputs[1], outputs[2]) > 1.0e-5,
                "filter oversampling modes were not distinct");
    }
}

void test_v124_performance_tools_and_library(const std::filesystem::path& root) {
    using namespace dve::audio;

    // Ratchets are scheduled at exact sample-frame subdivisions.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
        preset.arpeggiator.tempoBpm = 120.0F;
        preset.arpeggiator.gate = 0.4F;
        preset.arpeggiator.stepCount = 1U;
        preset.arpeggiator.steps[0].ratchets = 3U;
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F), "ratchet held note failed");
        std::vector<float> audio(6100U * 2U);
        synth.render(audio);
        std::vector<std::uint64_t> frames;
        MidiMessage output;
        while (synth.poll_midi_output(output)) if (output.is_note_on()) frames.push_back(output.sampleFrame);
        require(frames.size() >= 4U && frames[0] == 0U && frames[1] == 2000U &&
                frames[2] == 4000U && frames[3] == 6000U,
                "arpeggiator ratchets were not sample accurate");
    }

    // Game-clock tempo can replace the preset tempo without changing musical division logic.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.clockSource = ArpeggiatorClockSource::GameClock;
        preset.arpeggiator.externalTempoBpm = 120.0F;
        preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
        preset.arpeggiator.stepCount = 1U;
        synth.set_preset(preset);
        std::array<float, 2> warmup{};
        synth.render(warmup);
        synth.set_game_clock_tempo(240.0F);
        require(synth.note_on(60, 0.8F), "game-clock held note failed");
        std::vector<float> audio(6100U * 2U);
        synth.render(audio);
        std::vector<std::uint64_t> frames;
        MidiMessage output;
        while (synth.poll_midi_output(output)) if (output.is_note_on()) frames.push_back(output.sampleFrame);
        require(frames.size() >= 3U && frames[1] - frames[0] == 3000U && frames[2] - frames[1] == 3000U,
                "game-clock arpeggiator tempo was incorrect");
        require(synth.post_midi_clock(), "MIDI clock post was rejected");
    }

    // Scale-constrained chords are strummed at explicit sample offsets and emitted through MIDI output.
    {
        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.chord.enabled = true;
        preset.chord.type = ChordType::Major;
        preset.chord.scale = ChordScale::Major;
        preset.chord.scaleRoot = 0U;
        preset.chord.strumMilliseconds = 10.0F;
        preset.arpeggiator.sendMidiOutput = true;
        synth.set_preset(preset);
        require(synth.note_on(61, 1.0F), "scaled strummed chord note-on failed");
        std::vector<float> audio(1200U * 2U);
        synth.render(audio);
        std::vector<MidiMessage> notes;
        MidiMessage output;
        while (synth.poll_midi_output(output)) if (output.is_note_on()) notes.push_back(output);
        require(notes.size() == 3U && notes[0].data1 == 62U && notes[1].data1 == 65U && notes[2].data1 == 69U,
                "scale-constrained chord notes were incorrect");
        require(notes[0].sampleFrame == 0U && notes[1].sampleFrame == 480U && notes[2].sampleFrame == 960U,
                "chord strum timing was not sample accurate");
    }

    // Chord detection identifies pitch class, quality and inversion; memory slots remain user-editable.
    {
        const std::array<std::uint8_t, 4> notes{64U, 67U, 71U, 72U}; // C major 7, first inversion.
        const ChordDetection detected = detect_chord(notes);
        require(detected.matched && detected.type == ChordType::Major7 &&
                detected.rootPitchClass == 0U && detected.inversion == 1 && detected.confidence > 0.99F,
                "chord detection failed to identify a first-inversion major seventh");

        Synthesizer synth(48000);
        auto preset = focused_preset();
        preset.chord.enabled = true;
        preset.chord.useMemory = true;
        preset.chord.memorySlot = 0U;
        preset.chordMemory[0].name = "Power add 9";
        preset.chordMemory[0].intervals = {0, 7, 14, 19, 24, 31, 38, 43};
        preset.chordMemory[0].noteCount = 3U;
        synth.set_preset(preset);
        require(synth.note_on(48U, 1.0F), "chord-memory note-on failed");
        std::array<float, 2048> audio{};
        synth.render(audio.data(), audio.size() / 2U);
        std::set<unsigned> generated;
        MidiMessage output;
        while (synth.poll_midi_output(output)) if (output.is_note_on()) generated.insert(output.data1);
        require(generated == std::set<unsigned>({48U, 55U, 62U}),
                "user chord memory did not drive the realtime voicing");
    }

    // Standard MIDI File export contains the canonical header and a non-empty track.
    {
        auto preset = focused_preset();
        preset.arpeggiator.enabled = true;
        preset.arpeggiator.stepCount = 4U;
        preset.arpeggiator.steps[1].transpose = 3;
        preset.arpeggiator.steps[2].ratchets = 2U;
        preset.arpeggiator.steps[3].tie = true;
        std::string error;
        const auto midiPath = root / "arpeggiator.mid";
        require(export_arpeggiator_midi_file(preset, midiPath, 60U, &error), error.c_str());
        std::ifstream input(midiPath, std::ios::binary);
        std::array<char, 14> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        require(input.gcount() == static_cast<std::streamsize>(header.size()) &&
                std::string_view(header.data(), 4U) == "MThd" &&
                std::string_view(header.data() + 8U, 4U) == std::string_view("\0\0\0\1", 4U) &&
                std::filesystem::file_size(midiPath) > 40U,
                "arpeggiator MIDI export was malformed");
    }

    // Preset morphing and tag-based browsing are deterministic and range-valid.
    {
        auto a = focused_preset();
        a.name = "Soft Bass";
        a.filter.cutoffHertz = 400.0F;
        a.masterGain = 0.4F;
        auto b = focused_preset();
        b.name = "Bright Lead";
        b.filter.cutoffHertz = 6400.0F;
        b.masterGain = 0.8F;
        const auto midpoint = morph_synth_presets(a, b, 0.5F);
        std::string error;
        require(midpoint.validate(&error), error.c_str());
        require(midpoint.filter.cutoffHertz > 1500.0F && midpoint.filter.cutoffHertz < 1800.0F &&
                near(midpoint.masterGain, 0.6F), "preset morph did not interpolate parameters correctly");

        const auto libraryRoot = root / "presets";
        std::filesystem::create_directories(libraryRoot / "Bass" / "Analog");
        std::filesystem::create_directories(libraryRoot / "Lead");
        require(a.save(libraryRoot / "Bass" / "Analog" / "soft_bass.dvesynth", &error), error.c_str());
        require(b.save(libraryRoot / "Lead" / "bright_lead.dvesynth", &error), error.c_str());
        SynthPresetLibrary library;
        require(library.scan(libraryRoot, &error), error.c_str());
        require(library.entries().size() == 2U && library.find_by_tag("bass").size() == 1U &&
                library.find_by_tag("analog").size() == 1U && library.find_by_tag("lead").size() == 1U,
                "preset library tags were not indexed correctly");
    }
}


void test_packaged_preset_library() {
    using namespace dve::audio;
#ifdef DVE_SOURCE_DIR
    const std::filesystem::path root = std::filesystem::path(DVE_SOURCE_DIR) / "assets" / "audio" / "presets";
    SynthPresetLibrary library;
    std::string error;
    require(library.scan(root, &error), error.c_str());
    require(library.entries().size() >= 5U, "packaged preset library is incomplete");
    require(!library.find_by_tag("bass").empty() && !library.find_by_tag("wavetable").empty() &&
            !library.find_by_tag("hybrid").empty() && !library.find_by_tag("ms20").empty(),
            "packaged preset tags are incomplete");
    for (const auto& entry : library.entries()) {
        const auto preset = SynthPreset::load(entry.path, &error);
        require(preset.has_value(), error.c_str());
        require(preset->validate(&error), error.c_str());
    }
#endif
}

void test_polyphony_effects_and_preset(const std::filesystem::path& root) {
    using namespace dve::audio;
    Synthesizer synth(48000);
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Regression Eightfold Hybrid";
    preset.midiThru = true;
    for (auto& oscillator : preset.oscillators) oscillator.enabled = true;
    preset.oscillators[0].waveform = OscillatorWaveform::SuperSaw;
    preset.oscillators[0].pwmDepth = 0.37F;
    preset.oscillators[0].pwmRateHertz = 2.4F;
    preset.oscillators[0].shape = 0.61F;
    preset.oscillators[7].phaseOffset = 0.375F;
    preset.oscillators[7].keySync = false;
    preset.ampEnvelope = {0.021F, 0.31F, 0.42F, 0.57F, EnvelopeCurve::Linear, 0.013F, 0.027F};
    preset.filter.topology = FilterTopology::KorgMs20;
    preset.filter.mode = FilterMode::BandPass;
    preset.filter.alternateRevision = true;
    preset.filter.bassCompensation = 0.48F;
    preset.filter.morph = 0.33F;
    preset.filter.oversampling = FilterOversampling::X4;
    preset.filter.ms20HighPassCutoffHertz = 123.0F;
    preset.filter.selfOscillation = 1.12F;
    preset.filter.envelope = {0.02F, 0.31F, 0.42F, 0.57F, EnvelopeCurve::Linear, 0.017F, 0.029F};
    preset.tuning = {442.0F, -12.0F, 7.5F, 1.8F};
    preset.chord.enabled = true; preset.chord.type = ChordType::Minor9; preset.chord.inversion = 1;
    preset.chord.spreadOctaves = 2; preset.chord.velocityScale = 0.77F;
    preset.chord.scale = ChordScale::Dorian; preset.chord.scaleRoot = 2U; preset.chord.strumMilliseconds = 17.0F;
    preset.chord.useMemory = true; preset.chord.memorySlot = 3U;
    preset.chordMemory[3].name = "Wide minor ninth";
    preset.chordMemory[3].intervals = {0, 3, 10, 14, 19, 24, 27, 34};
    preset.chordMemory[3].noteCount = 5U;
    preset.arpeggiator.enabled = true; preset.arpeggiator.latch = true;
    preset.arpeggiator.mode = ArpeggiatorMode::UpDown;
    preset.arpeggiator.division = ArpeggiatorDivision::EighthTriplet;
    preset.arpeggiator.tempoBpm = 137.0F; preset.arpeggiator.gate = 0.73F;
    preset.arpeggiator.swing = 0.21F; preset.arpeggiator.octaveRange = 3;
    preset.arpeggiator.stepCount = 12; preset.arpeggiator.steps[3].transpose = 7;
    preset.arpeggiator.steps[3].probability = 0.65F;
    preset.distortion = {true, 2.7F, 0.24F}; preset.eq = {true, 2.0F, -1.5F, 1.0F};
    preset.chorus = {true, 0.47F, 5.5F, 0.21F}; preset.phaser = {true, 0.28F, 0.74F, 0.31F, 0.17F};
    preset.delay = {true, 0.27F, 0.37F, 0.19F, false}; preset.reverb = {true, 0.71F, 0.51F, 0.67F, 0.23F};
    preset.compressor = {true, -15.0F, 4.0F, 6.0F, 120.0F, 2.0F}; preset.limiter = {true, -0.7F, 60.0F};
    std::string error;
    require(preset.validate(&error), error.c_str());
    const std::string serialized = preset.serialize();
    require(serialized.starts_with("DVE_SYNTH_PRESET=5\n"), "current preset format did not advance to version 5");
    const auto decoded = SynthPreset::parse(serialized, &error);
    require(decoded && decoded->name == preset.name &&
            decoded->oscillators[0].waveform == preset.oscillators[0].waveform &&
            near(decoded->oscillators[0].pwmDepth, preset.oscillators[0].pwmDepth) &&
            near(decoded->ampEnvelope.delaySeconds, preset.ampEnvelope.delaySeconds) &&
            near(decoded->ampEnvelope.holdSeconds, preset.ampEnvelope.holdSeconds) &&
            decoded->filter.topology == preset.filter.topology && decoded->filter.alternateRevision &&
            decoded->filter.oversampling == FilterOversampling::X4 &&
            near(decoded->filter.ms20HighPassCutoffHertz, 123.0F) &&
            near(decoded->filter.envelope.holdSeconds, 0.029F) &&
            near(decoded->tuning.referenceHertz, preset.tuning.referenceHertz) &&
            decoded->chord.type == preset.chord.type && decoded->chord.inversion == preset.chord.inversion &&
            decoded->chord.scale == ChordScale::Dorian && decoded->chord.useMemory && decoded->chord.memorySlot == 3U &&
            decoded->chordMemory[3].name == "Wide minor ninth" && decoded->chordMemory[3].noteCount == 5U &&
            decoded->chordMemory[3].intervals[3] == 14 &&
            decoded->arpeggiator.mode == preset.arpeggiator.mode &&
            decoded->arpeggiator.division == preset.arpeggiator.division &&
            near(decoded->arpeggiator.steps[3].probability, 0.65F) &&
            near(decoded->eq.midGainDb, preset.eq.midGainDb) && decoded->delay.pingPong == preset.delay.pingPong,
            "complete version-3 synth preset round trip failed");
    const auto legacy = SynthPreset::parse("DVE_SYNTH_PRESET=1\nname=Legacy\nosc0.wave=sine\n", &error);
    require(legacy && legacy->name == "Legacy" && legacy->oscillators[0].waveform == OscillatorWaveform::Sine,
            "version-1 preset migration failed");
    const auto version2 = SynthPreset::parse("DVE_SYNTH_PRESET=2\nname=Version Two\namp.attack=0.25\n", &error);
    require(version2 && version2->name == "Version Two" && near(version2->ampEnvelope.attackSeconds, 0.25F) &&
            near(version2->ampEnvelope.delaySeconds, 0.0F), "version-2 preset migration failed");
    auto invalid = preset; invalid.arpeggiator.tempoBpm = 700.0F;
    require(!invalid.validate(&error), "invalid arpeggiator parameters were accepted");
    require(decoded->save(root / "regression.dvesynth", &error), error.c_str());
    const auto loaded = SynthPreset::load(root / "regression.dvesynth", &error);
    require(loaded && loaded->name == preset.name, "synth preset file load failed");

    auto polyPreset = *loaded;
    polyPreset.chord.enabled = false; polyPreset.arpeggiator.enabled = false;
    synth.set_preset(polyPreset);
    for (std::uint8_t note = 48; note < 68; ++note) require(synth.note_on(note, 0.7F), "note-on queue failed");
    std::vector<float> audio(48000U * 2U);
    synth.render(audio.data(), 24000);
    const auto meter = synth.meters();
    require(meter.activeVoices == 16, "16-voice polyphony limit was not enforced");
    require(meter.peakLeft > 0.005F && meter.peakRight > 0.005F, "synth produced silence");
    require(std::all_of(audio.begin(), audio.end(), [](float sample) { return std::isfinite(sample); }),
            "synth produced non-finite audio");
    require(*std::max_element(audio.begin(), audio.end()) <= 1.01F, "limiter did not contain output");

    require(synth.control_change(64, 127), "sustain-on failed");
    for (std::uint8_t note = 48; note < 68; ++note) require(synth.note_off(note), "note-off queue failed");
    synth.render(audio.data(), 2048);
    require(synth.meters().activeVoices > 0, "sustain pedal did not retain voices");
    require(synth.control_change(64, 0), "sustain-off failed");
    synth.render(audio.data(), 24000); synth.render(audio.data() + 48000, 24000);
    require(synth.meters().activeVoices < 16, "released voices did not decay");
    require(write_float_wav(root / "synth_regression.wav", audio, synth.sample_rate(), &error), error.c_str());
    require(std::filesystem::file_size(root / "synth_regression.wav") > 1000U, "WAV regression render missing");
}

void test_v125_expression_microtuning_and_authoring(const std::filesystem::path& root) {
    using namespace dve::audio;

    // Version-4 state must preserve expressive-performance and quality settings.
    {
        SynthPreset preset = focused_preset();
        preset.name = "v1.25 Expressive Round Trip";
        preset.oscillatorQuality = OscillatorQuality::Offline;
        preset.filterQuality = FilterQuality::High;
        preset.unison = {true, 4, 17.5F, 0.82F, 0.31F, true};
        preset.mpe.zoneMode = MpeZoneMode::Lower;
        preset.mpe.memberPitchBendRangeSemitones = 48.0F;
        preset.microtuning = MicrotuningTable::equal_temperament(442.0F, 69);
        preset.microtuning.enabled = true;
        preset.microtuning.name = "Concert A 442";
        preset.microtuning.centsOffset[60] = 13.7F;
        preset.modulation[0] = {true, ModulationSource::Velocity,
                                ModulationDestination::FilterCutoff, 0.75F,
                                ModulationCurve::Quadratic, ModulationPolarity::Unipolar, 42.0F};
        preset.arpeggiator.steps[0].condition = ArpeggiatorCondition::Fill;
        preset.arpeggiator.steps[0].automationCurve = StepAutomationCurve::Smooth;
        preset.arpeggiator.steps[0].accent = true;
        preset.arpeggiator.steps[0].slide = true;
        preset.arpeggiator.steps[0].macro3 = 0.37F;
        preset.arpeggiator.steps[0].macro4 = 0.81F;
        preset.metadata.author = "DVE Audio";
        preset.metadata.category = "Expressive";
        preset.metadata.version = "1.25";
        preset.metadata.tags[0] = "mpe";
        preset.metadata.tagCount = 1;
        std::string error;
        const std::string encoded = preset.serialize();
        require(encoded.starts_with("DVE_SYNTH_PRESET=5\n"), "physical-model preset version header missing");
        const auto decoded = SynthPreset::parse(encoded, &error);
        require(decoded.has_value(), error.c_str());
        require(decoded->oscillatorQuality == OscillatorQuality::Offline &&
                decoded->filterQuality == FilterQuality::High,
                "quality settings did not round trip");
        require(decoded->unison.enabled && decoded->unison.voices == 4 &&
                near(decoded->unison.detuneCents, 17.5F), "unison did not round trip");
        require(decoded->mpe.zoneMode == MpeZoneMode::Lower &&
                near(decoded->mpe.memberPitchBendRangeSemitones, 48.0F),
                "MPE configuration did not round trip");
        require(decoded->microtuning.enabled && near(decoded->microtuning.referenceHertz, 442.0F) &&
                near(decoded->microtuning.centsOffset[60], 13.7F),
                "microtuning did not round trip");
        require(decoded->modulation[0].polarity == ModulationPolarity::Unipolar &&
                near(decoded->modulation[0].smoothingMilliseconds, 42.0F),
                "modulation extensions did not round trip");
        require(decoded->arpeggiator.steps[0].condition == ArpeggiatorCondition::Fill &&
                decoded->arpeggiator.steps[0].automationCurve == StepAutomationCurve::Smooth &&
                decoded->arpeggiator.steps[0].accent && decoded->arpeggiator.steps[0].slide &&
                near(decoded->arpeggiator.steps[0].macro4, 0.81F),
                "sequencer extensions did not round trip");
        require(decoded->metadata.author == "DVE Audio" && decoded->metadata.tagCount == 1,
                "preset metadata did not round trip");
    }

    // MPE member channels must retain independent bend, pressure and timbre while sharing master bend.
    {
        SynthPreset preset = focused_preset();
        preset.mpe.zoneMode = MpeZoneMode::Lower;
        preset.mpe.lowerMasterChannel = 0;
        preset.mpe.lowerMemberCount = 15;
        preset.mpe.masterPitchBendRangeSemitones = 2.0F;
        preset.mpe.memberPitchBendRangeSemitones = 48.0F;
        Synthesizer synth(48000);
        synth.set_preset(preset);
        require(synth.note_on(60, 0.8F, 1), "MPE member-1 note failed");
        require(synth.note_on(64, 0.7F, 2), "MPE member-2 note failed");
        require(synth.pitch_bend(4096, 0), "MPE master bend failed");
        require(synth.pitch_bend(4096, 1), "MPE member bend failed");
        const std::array<std::uint8_t, 2> pressureBytes{0xD1U, 96U};
        const auto pressure = MidiMessage::parse(pressureBytes);
        require(pressure && synth.post_midi(*pressure), "MPE pressure message failed");
        require(synth.control_change(74, 110, 1), "MPE timbre message failed");
        std::array<float, 1024> block{};
        synth.render(block);
        const auto voices = synth.voices();
        const SynthVoiceInfo* member1 = nullptr;
        const SynthVoiceInfo* member2 = nullptr;
        for (const auto& voice : voices) {
            if (voice.active && voice.channel == 1 && voice.note == 60) member1 = &voice;
            if (voice.active && voice.channel == 2 && voice.note == 64) member2 = &voice;
        }
        require(member1 && member2, "MPE voices were not independently allocated");
        require(member1->pitchBendSemitones > 24.5F && member1->pitchBendSemitones < 25.5F,
                "MPE master/member bend composition is incorrect");
        require(member2->pitchBendSemitones > 0.9F && member2->pitchBendSemitones < 1.1F,
                "MPE master bend did not reach another member");
        require(member1->pressure > 0.74F && member1->timbre > 0.85F,
                "MPE pressure or timbre was not voice-local");
        require(member2->pressure < 0.01F && member2->timbre < 0.01F,
                "MPE expression leaked between member channels");

        require(synth.control_change(64, 127, 0), "MPE master sustain-on failed");
        require(synth.note_off(60, 0.0F, 1) && synth.note_off(64, 0.0F, 2),
                "MPE note-off failed");
        synth.render(block);
        require(synth.meters().activeVoices >= 2U, "MPE master sustain did not retain member notes");
        require(synth.control_change(64, 0, 0), "MPE master sustain-off failed");
    }

    // Scala and MIDI Tuning Standard imports must produce stable, measurable tuning tables.
    {
        const auto sclPath = root / "twelve_equal.scl";
        std::ofstream scl(sclPath);
        scl << "! test scale\nTwelve Equal\n12\n";
        for (int step = 1; step <= 12; ++step) scl << step * 100 << ".0\n";
        scl.close();
        std::string error;
        const auto scala = import_scala_tuning(sclPath, std::nullopt, &error);
        require(scala.has_value(), error.c_str());
        require(scala->enabled && std::abs(scala->centsOffset[21]) < 1.0e-3F &&
                std::abs(scala->centsOffset[108]) < 1.0e-3F,
                "12-EDO Scala import did not reproduce equal temperament");
        require(std::abs(scala->frequency(69) - 440.0F) < 0.01F,
                "Scala reference frequency is incorrect");

        MicrotuningTable mts = MicrotuningTable::equal_temperament();
        const std::array<std::uint8_t, 12> message{
            0xF0U, 0x7FU, 0x7FU, 0x08U, 0x02U, 0x00U, 0x01U,
            60U, 60U, 64U, 0U, 0xF7U};
        require(apply_midi_tuning_standard(message, mts, &error), error.c_str());
        require(mts.enabled && std::abs(mts.centsOffset[60] - 50.0F) < 0.01F,
                "MIDI Tuning Standard single-note update failed");
        require(mts.frequency(60) > MicrotuningTable::equal_temperament().frequency(60) * 1.028F,
                "MIDI Tuning Standard frequency did not move upward");
    }

    // Wavetable authoring and chord capture must be deterministic and bounded.
    {
        WavetableBank table = WavetableBank::make_default();
        std::array<float, 17> drawn{};
        for (std::size_t i = 0; i < drawn.size(); ++i)
            drawn[i] = 0.4F + std::sin(static_cast<float>(i) * 2.0F * 3.14159265F /
                                       static_cast<float>(drawn.size() - 1U));
        std::string error;
        require(wavetable_draw_frame(table, 4, drawn, &error), error.c_str());
        require(wavetable_remove_dc(table) && wavetable_normalize(table) && wavetable_align_phases(table),
                "wavetable cleanup failed");
        require(wavetable_spectral_morph(table, 0, 4, 5, 0.35F, &error), error.c_str());
        require(table.frameCount >= 6U && table.contentHash != 0U && table.validate(&error),
                "authored wavetable is invalid");

        SynthPreset preset = focused_preset();
        const std::array<std::uint8_t, 4> notes{67U, 60U, 64U, 72U};
        require(capture_chord_memory(preset, 2, notes, "C Major Octave", &error), error.c_str());
        require(preset.chordMemory[2].noteCount == 4U && preset.chordMemory[2].intervals[0] == 0 &&
                preset.chordMemory[2].intervals[1] == 4 && preset.chordMemory[2].intervals[2] == 7 &&
                preset.chordMemory[2].intervals[3] == 12,
                "captured chord memory was not sorted and normalized");
    }

    // Quality, unison, and modulation routes must all affect a finite rendered signal.
    {
        SynthPreset base = focused_preset();
        base.oscillators[0].waveform = OscillatorWaveform::Pulse;
        base.oscillators[0].pulseWidth = 0.23F;
        base.oscillators[0].pwmDepth = 0.55F;
        base.filter.enabled = true;
        base.filter.topology = FilterTopology::KorgMs20;
        base.filter.cutoffHertz = 2300.0F;
        base.filter.drive = 2.5F;
        base.filter.resonance = 0.74F;
        SynthPreset high = base;
        high.oscillatorQuality = OscillatorQuality::High;
        high.filterQuality = FilterQuality::High;
        SynthPreset unison = high;
        unison.unison = {true, 4, 18.0F, 1.0F, 0.25F, true};
        const auto normalAudio = render_note(base, 4096U);
        const auto highAudio = render_note(high, 4096U);
        const auto unisonAudio = render_note(unison, 4096U);
        require(mean_abs_difference(normalAudio, highAudio) > 1.0e-5,
                "quality modes did not change the signal");
        require(mean_abs_difference(highAudio, unisonAudio) > 1.0e-4,
                "unison did not change the signal");

        SynthPreset modulated = focused_preset();
        modulated.modulation[0] = {true, ModulationSource::Velocity,
                                   ModulationDestination::VoicePan, 0.8F,
                                   ModulationCurve::Linear, ModulationPolarity::Unipolar, 25.0F};
        Synthesizer synth(48000);
        synth.set_preset(modulated);
        require(synth.note_on(60, 0.9F), "modulation test note failed");
        std::array<float, 1024> block{};
        synth.render(block);
        const auto activity = synth.modulation_activity();
        require(activity[0].enabled && activity[0].source == ModulationSource::Velocity &&
                activity[0].destination == ModulationDestination::VoicePan &&
                std::abs(activity[0].currentValue) > 0.05F,
                "modulation activity telemetry did not report the active route");
    }
}


void test_v126_sample_and_granular_oscillators(const std::filesystem::path& root) {
    using namespace dve::audio;
    SynthSampleBank bank;
    bank.name = "Deterministic Vocal Grain";
    bank.enabled = true;
    bank.sampleRate = 48000;
    bank.rootNote = 60;
    bank.frameCount = 4096;
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < bank.frameCount; ++i) {
        const float t = static_cast<float>(i) / 48000.0F;
        const float envelope = 1.0F - static_cast<float>(i) / static_cast<float>(bank.frameCount);
        bank.samples[i] = (0.65F * std::sin(2.0F * 3.14159265F * 261.6256F * t) +
                           0.25F * std::sin(2.0F * 3.14159265F * 523.2511F * t)) * envelope;
        const auto bits = std::bit_cast<std::uint32_t>(bank.samples[i]);
        hash ^= bits; hash *= 1099511628211ULL;
    }
    bank.contentHash = hash;
    std::string error;
    require(bank.validate(&error), error.c_str());

    SynthPreset samplePreset = focused_preset();
    samplePreset.name = "Resident Sample Regression";
    samplePreset.sampleBank = bank;
    auto& sampleOsc = samplePreset.oscillators[0];
    sampleOsc.waveform = OscillatorWaveform::Sample;
    sampleOsc.sampleLoop = true;
    sampleOsc.sampleLoopStart = 0.12F;
    sampleOsc.sampleLoopEnd = 0.86F;
    sampleOsc.sampleEnd = 0.92F;
    sampleOsc.sampleVelocityToGain = 0.6F;
    const auto sampleAudio = render_note(samplePreset, 8192U);
    require(rms(sampleAudio) > 0.01, "resident sample oscillator rendered silence");

    SynthPreset reversePreset = samplePreset;
    reversePreset.oscillators[0].sampleReverse = true;
    const auto reverseAudio = render_note(reversePreset, 8192U);
    require(mean_abs_difference(sampleAudio, reverseAudio) > 0.001,
            "reverse sample playback did not alter the signal");

    SynthPreset granularPreset = samplePreset;
    auto& grain = granularPreset.oscillators[0];
    grain.waveform = OscillatorWaveform::Granular;
    grain.grainSizeMilliseconds = 48.0F;
    grain.grainDensityHertz = 36.0F;
    grain.grainSpray = 0.28F;
    grain.grainPitchSemitones = 7.0F;
    grain.grainStereoSpread = 0.9F;
    grain.grainWindow = GrainWindow::Tukey;
    const auto granularAudioA = render_note(granularPreset, 8192U);
    const auto granularAudioB = render_note(granularPreset, 8192U);
    require(rms(granularAudioA) > 0.005, "granular oscillator rendered silence");
    require(granularAudioA == granularAudioB, "granular scheduler was not deterministic");
    require(mean_abs_difference(sampleAudio, granularAudioA) > 0.001,
            "granular oscillator was indistinguishable from sample playback");

    const auto path = root / "sample_granular_v5.dvesynth";
    require(granularPreset.save(path, &error), error.c_str());
    const auto loaded = SynthPreset::load(path, &error);
    require(loaded.has_value(), error.c_str());
    require(loaded->sampleBank.enabled && loaded->sampleBank.frameCount == bank.frameCount &&
            loaded->sampleBank.contentHash == bank.contentHash &&
            loaded->oscillators[0].waveform == OscillatorWaveform::Granular &&
            near(loaded->oscillators[0].grainDensityHertz, 36.0F),
            "sample/granular preset did not round trip");
}

}

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "dve_audio_synth_tests";
        std::filesystem::remove_all(root); std::filesystem::create_directories(root);
        test_midi_codec_and_ports();
        test_waveforms_pwm_and_filters();
        test_chords_and_arpeggiator();
        test_v124_modulation_wavetable_and_filters(root);
        test_v124_performance_tools_and_library(root);
        test_v125_expression_microtuning_and_authoring(root);
        test_v126_sample_and_granular_oscillators(root);
        test_packaged_preset_library();
        test_polyphony_effects_and_preset(root);
        std::cout << "dve_audio_synth_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_audio_synth_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
