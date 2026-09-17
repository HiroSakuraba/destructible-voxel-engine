#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {
struct Options {
    std::filesystem::path wav{"dve_eightfold_expressive_demo.wav"};
    std::filesystem::path preset{"dve_eightfold_expressive_demo.dvesynth"};
    std::filesystem::path report{"dve_eightfold_expressive_demo.json"};
    std::filesystem::path midi{"dve_eightfold_expressive_arpeggiator.mid"};
};
}

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--wav" && i + 1 < argc) options.wav = argv[++i];
        else if (argument == "--preset" && i + 1 < argc) options.preset = argv[++i];
        else if (argument == "--report" && i + 1 < argc) options.report = argv[++i];
        else if (argument == "--midi" && i + 1 < argc) options.midi = argv[++i];
        else {
            std::cerr << "usage: dve_hybrid_synth_demo [--wav PATH] [--preset PATH] "
                         "[--report PATH] [--midi PATH]\n";
            return 2;
        }
    }

    using namespace dve::audio;
    constexpr std::uint32_t sampleRate = 48000U;
    constexpr std::size_t sectionFrames = sampleRate * 2U;
    constexpr std::size_t blockFrames = 256U;
    Synthesizer synth(sampleRate);
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Eightfold Expressive Motion Tour";
    preset.masterGain = 0.54F;
    preset.tuning.analogDriftCents = 1.25F;
    preset.oscillatorQuality = OscillatorQuality::High;
    preset.filterQuality = FilterQuality::High;
    preset.unison = {true, 2U, 11.5F, 0.72F, 0.19F, true};
    preset.mpe.zoneMode = MpeZoneMode::Lower;
    preset.mpe.memberPitchBendRangeSemitones = 48.0F;
    preset.microtuning = MicrotuningTable::equal_temperament();
    preset.microtuning.enabled = true;
    preset.microtuning.name = "DVE expressive color tuning";
    static constexpr std::array<float, 12> pitchClassOffsets{
        0.0F, -11.7F, 3.9F, 15.6F, -13.7F, -2.0F,
        -9.8F, 2.0F, 13.7F, -15.6F, 17.6F, -11.7F};
    for (std::size_t note = 0; note < preset.microtuning.centsOffset.size(); ++note)
        preset.microtuning.centsOffset[note] = pitchClassOffsets[note % 12U];
    preset.metadata.author = "DVE Audio";
    preset.metadata.category = "Expressive Sequence";
    preset.metadata.version = "1.25";
    preset.metadata.tags[0] = "mpe";
    preset.metadata.tags[1] = "microtuned";
    preset.metadata.tags[2] = "unison";
    preset.metadata.tagCount = 3U;
    preset.ampEnvelope = {0.006F, 0.18F, 0.72F, 0.28F, EnvelopeCurve::Exponential, 0.0F, 0.012F};
    preset.filter.cutoffHertz = 1650.0F;
    preset.filter.resonance = 0.61F;
    preset.filter.drive = 2.15F;
    preset.filter.envelopeAmountOctaves = 2.0F;
    preset.filter.envelope = {0.008F, 0.27F, 0.20F, 0.25F, EnvelopeCurve::Exponential, 0.0F, 0.02F};
    preset.filter.oversampling = FilterOversampling::X2;
    preset.filter.ms20HighPassCutoffHertz = 72.0F;
    preset.filter.selfOscillation = 0.96F;

    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.wavetable.enabled = true;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].waveform = OscillatorWaveform::Wavetable;
    preset.oscillators[0].gain = 0.22F;
    preset.oscillators[0].wavetablePosition = 0.15F;
    preset.oscillators[1].enabled = true;
    preset.oscillators[1].waveform = OscillatorWaveform::SuperSaw;
    preset.oscillators[1].gain = 0.10F;
    preset.oscillators[1].hardSyncSource = 0;
    preset.oscillators[1].semitones = 12.0F;
    preset.oscillators[2].enabled = true;
    preset.oscillators[2].waveform = OscillatorWaveform::Pulse;
    preset.oscillators[2].gain = 0.13F;
    preset.oscillators[2].semitones = -12.0F;
    preset.oscillators[2].pulseWidth = 0.38F;
    preset.oscillators[2].pwmDepth = 0.62F;
    preset.oscillators[2].pwmRateHertz = 2.2F;
    preset.oscillators[2].subOscillatorLevel = 0.22F;
    preset.oscillators[3].enabled = true;
    preset.oscillators[3].waveform = OscillatorWaveform::FoldedSine;
    preset.oscillators[3].gain = 0.08F;
    preset.oscillators[3].frequencyModSource = 0;
    preset.oscillators[3].frequencyModMode = FrequencyModulationMode::Exponential;
    preset.oscillators[3].frequencyModAmount = 0.18F;
    preset.oscillators[4].enabled = true;
    preset.oscillators[4].waveform = OscillatorWaveform::Digital;
    preset.oscillators[4].gain = 0.045F;
    preset.oscillators[4].ringModSource = 3;
    preset.oscillators[4].ringModDepth = 0.38F;

    preset.lfos[0] = {true, LfoWaveform::Triangle, 4.6F, 1.0F, 0.0F, 0.08F, true, false, 1.0F};
    preset.lfos[1] = {true, LfoWaveform::SmoothRandom, 0.42F, 1.0F, 0.37F, 0.0F, true, true, 4.0F};
    preset.modulation[0] = {true, ModulationSource::Lfo1, ModulationDestination::Osc3PulseWidth,
                            0.82F, ModulationCurve::Linear, ModulationPolarity::Bipolar, 10.0F};
    preset.modulation[1] = {true, ModulationSource::Lfo2, ModulationDestination::FilterCutoff,
                            0.45F, ModulationCurve::Cubic};
    preset.modulation[2] = {true, ModulationSource::FilterEnvelope, ModulationDestination::Osc1Shape,
                            0.52F, ModulationCurve::Quadratic};
    preset.modulation[3] = {true, ModulationSource::Macro1, ModulationDestination::FilterDrive,
                            0.38F, ModulationCurve::Linear};
    preset.modulation[4] = {true, ModulationSource::Velocity, ModulationDestination::VoiceGain,
                            0.22F, ModulationCurve::Linear};
    preset.macros.names[0] = "Motion";
    preset.macros.values[0] = 0.58F;
    preset.midiLearn[0] = {true, 74U, 0U, 0.0F, 1.0F, false};

    preset.chord.enabled = true;
    preset.chord.useMemory = true;
    preset.chord.memorySlot = 3U;
    preset.chord.inversion = 1;
    preset.chord.spreadOctaves = 1;
    preset.chord.scale = ChordScale::Dorian;
    preset.chord.scaleRoot = 2U;
    preset.chord.strumMilliseconds = 13.0F;
    preset.chord.velocityScale = 0.82F;
    preset.chordMemory[3].name = "Dorian minor ninth";
    preset.chordMemory[3].intervals = {0, 3, 7, 10, 14, 19, 24, 27};
    preset.chordMemory[3].noteCount = 5U;

    preset.arpeggiator.enabled = true;
    preset.arpeggiator.mode = ArpeggiatorMode::UpDown;
    preset.arpeggiator.division = ArpeggiatorDivision::Sixteenth;
    preset.arpeggiator.tempoBpm = 116.0F;
    preset.arpeggiator.gate = 0.68F;
    preset.arpeggiator.swing = 0.10F;
    preset.arpeggiator.octaveRange = 2U;
    preset.arpeggiator.stepCount = 8U;
    preset.arpeggiator.steps[0].accent = true;
    preset.arpeggiator.steps[1].ratchets = 2U;
    preset.arpeggiator.steps[1].automationCurve = StepAutomationCurve::Linear;
    preset.arpeggiator.steps[2].transpose = 7;
    preset.arpeggiator.steps[3].velocityScale = 0.72F;
    preset.arpeggiator.steps[3].macro1 = 0.85F;
    preset.arpeggiator.steps[3].macro3 = 0.65F;
    preset.arpeggiator.steps[3].slide = true;
    preset.arpeggiator.steps[4].tie = true;
    preset.arpeggiator.steps[5].octaveOffset = 1;
    preset.arpeggiator.steps[6].ratchets = 3U;
    preset.arpeggiator.steps[7].gateScale = 0.52F;
    preset.arpeggiator.steps[7].macro2 = 0.25F;
    preset.arpeggiator.steps[7].macro4 = 0.78F;
    preset.arpeggiator.steps[7].condition = ArpeggiatorCondition::Every2;

    preset.delay.enabled = true; preset.delay.timeSeconds = 0.24F; preset.delay.mix = 0.15F;
    preset.reverb.enabled = true; preset.reverb.mix = 0.18F;
    preset.chorus.enabled = true; preset.chorus.mix = 0.10F;
    preset.distortion.enabled = true; preset.distortion.drive = 1.6F; preset.distortion.mix = 0.08F;

    std::string error;
    if (!preset.validate(&error) || !preset.save(options.preset, &error) ||
        !export_arpeggiator_midi_file(preset, options.midi, 50U, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    static constexpr std::array topologies{
        FilterTopology::MoogLadder, FilterTopology::KorgMs20,
        FilterTopology::OberheimSem, FilterTopology::CleanStateVariable};
    std::vector<float> output;
    output.reserve((topologies.size() * sectionFrames + sampleRate) * 2U);
    std::vector<float> block(blockFrames * 2U);

    for (std::size_t section = 0; section < topologies.size(); ++section) {
        preset.filter.topology = topologies[section];
        preset.filter.mode = section == 1U ? FilterMode::BandPass : FilterMode::LowPass;
        preset.filter.morph = section == 2U ? 0.42F : 0.0F;
        preset.filter.alternateRevision = section == 1U;
        preset.filter.cutoffHertz = 1150.0F + static_cast<float>(section) * 580.0F;
        preset.oscillators[0].wavetablePosition = 0.12F + 0.24F * static_cast<float>(section);
        preset.macros.values[0] = 0.30F + 0.20F * static_cast<float>(section);
        synth.set_preset(preset);
        if (section == 0U) {
            (void)synth.note_on(50U, 0.88F, 1U);
            (void)synth.note_on(57U, 0.72F, 2U);
        }
        (void)synth.pitch_bend(static_cast<std::int16_t>(-2048 + static_cast<int>(section) * 1365), 1U);
        (void)synth.pitch_bend(static_cast<std::int16_t>(2048 - static_cast<int>(section) * 1024), 2U);
        (void)synth.control_change(74U, static_cast<std::uint8_t>(40U + section * 24U), 1U);
        const std::array<std::uint8_t, 2> pressureBytes{
            static_cast<std::uint8_t>(0xD0U | 2U), static_cast<std::uint8_t>(55U + section * 18U)};
        if (const auto pressure = MidiMessage::parse(pressureBytes)) (void)synth.post_midi(*pressure);
        std::size_t rendered = 0U;
        while (rendered < sectionFrames) {
            const std::size_t frames = std::min(blockFrames, sectionFrames - rendered);
            synth.render(block.data(), frames);
            output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(frames * 2U));
            rendered += frames;
        }
    }
    (void)synth.note_off(50U, 0.0F, 1U);
    (void)synth.note_off(57U, 0.0F, 2U);
    for (std::size_t i = 0; i < sampleRate / blockFrames; ++i) {
        synth.render(block);
        output.insert(output.end(), block.begin(), block.end());
    }

    if (!write_float_wav(options.wav, output, sampleRate, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    float peakLeft = 0.0F;
    float peakRight = 0.0F;
    for (std::size_t frame = 0; frame + 1U < output.size(); frame += 2U) {
        peakLeft = std::max(peakLeft, std::abs(output[frame]));
        peakRight = std::max(peakRight, std::abs(output[frame + 1U]));
    }
    std::ofstream report(options.report, std::ios::binary | std::ios::trunc);
    report << "{\n"
           << "  \"demo\": \"DVE Eightfold Expressive modulation, MPE and microtuning tour\",\n"
           << "  \"sample_rate_hz\": " << sampleRate << ",\n"
           << "  \"duration_seconds\": " << static_cast<double>(output.size() / 2U) / sampleRate << ",\n"
           << "  \"filter_order\": [\"Moog ladder style\", \"Korg MS-20 style\", \"Oberheim SEM style\", \"clean state-variable\"],\n"
           << "  \"oscillator_paths\": [\"wavetable morph\", \"hard sync\", \"PWM\", \"exponential FM\", \"ring modulation\", \"sub oscillator\"],\n"
           << "  \"modulation\": \"two LFOs, smoothed bipolar routes, four macro lanes and MIDI learn\",\n"
           << "  \"chord\": \"user-memory Dorian minor ninth, first inversion, strummed\",\n"
           << "  \"arpeggiator\": \"Up/down, 1/16, 116 BPM, swing, conditions, accent, slide, ratchets, tie and four macro lanes\",\n"
           << "  \"expression\": \"lower-zone MPE with independent bend, pressure and CC74 timbre\",\n"
           << "  \"tuning\": \"per-note expressive color table around A440\",\n"
           << "  \"unison_voices\": 2,\n"
           << "  \"midi_file\": \"" << options.midi.filename().string() << "\",\n"
           << "  \"peak_left\": " << peakLeft << ",\n"
           << "  \"peak_right\": " << peakRight << "\n"
           << "}\n";
    if (!report) return 1;
    std::cout << options.wav << '\n';
    return 0;
}
