#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include "dve/audio/sample_map.hpp"
#include "dve/audio/synthesizer.hpp"

namespace {
using namespace dve::audio;

constexpr std::uint32_t kSourceFrames = 12000U;

float glass_source(std::uint32_t frame, float fundamental, float phaseOffset, float decay) {
    const float t = static_cast<float>(frame) / 48000.0F;
    const float fade = std::min(1.0F, t * 40.0F) * std::exp(-t * decay);
    const float shimmer = 0.58F * std::sin(2.0F * std::numbers::pi_v<float> * fundamental * t + phaseOffset) +
                          0.24F * std::sin(2.0F * std::numbers::pi_v<float> * fundamental * 2.0F * t + 0.3F) +
                          0.12F * std::sin(2.0F * std::numbers::pi_v<float> * fundamental * 3.0F * t + 1.2F);
    return shimmer * fade;
}

SynthSampleMap make_sample_map() {
    SynthSampleMap map;
    map.name = "DVE Granular Glass Choir Map";
    map.sourceCount = 3U;
    map.zoneCount = 3U;
    map.residentFrameCount = kSourceFrames * 3U;

    for (std::uint32_t sourceIndex = 0; sourceIndex < map.sourceCount; ++sourceIndex) {
        auto& source = map.sources[sourceIndex];
        source.name = sourceIndex == 0U ? "Glass A" : (sourceIndex == 1U ? "Glass B" : "Glass Release");
        source.preload = SamplePreloadPolicy::Resident;
        source.sampleRate = 48000U;
        source.totalFrameCount = kSourceFrames;
        source.residentFrameOffset = sourceIndex * kSourceFrames;
        source.residentFrameCount = kSourceFrames;
    }

    for (std::uint32_t frame = 0; frame < kSourceFrames; ++frame) {
        map.residentSamples[frame] = glass_source(frame, 261.6256F, 0.0F, 3.2F);
        map.residentSamples[kSourceFrames + frame] = glass_source(frame, 261.6256F, 0.42F, 2.8F) * 0.92F;
        const float t = static_cast<float>(frame) / 48000.0F;
        const float envelope = std::exp(-t * 13.0F);
        map.residentSamples[kSourceFrames * 2U + frame] = envelope *
            (0.42F * std::sin(2.0F * std::numbers::pi_v<float> * 1046.502F * t) +
             0.18F * std::sin(2.0F * std::numbers::pi_v<float> * 1567.982F * t + 0.7F));
    }

    for (std::uint8_t index = 0U; index < 2U; ++index) {
        auto& zone = map.zones[index];
        zone.sourceIndex = index;
        zone.keyLow = 0U;
        zone.keyHigh = 127U;
        zone.velocityLow = 1U;
        zone.velocityHigh = 127U;
        zone.rootNote = 60U;
        zone.roundRobinGroup = 1U;
        zone.roundRobinIndex = index;
        zone.roundRobinCount = 2U;
        zone.gain = index == 0U ? 1.0F : 0.94F;
        zone.pan = index == 0U ? -0.08F : 0.08F;
        zone.startFrame = 0U;
        zone.endFrame = kSourceFrames;
        zone.loopMode = SampleLoopMode::Forward;
        zone.loopStartFrame = 1200U;
        zone.loopEndFrame = 9600U;
        zone.loopCrossfadeFrames = 256U;
    }

    auto& release = map.zones[2];
    release.trigger = SampleTrigger::Release;
    release.sourceIndex = 2U;
    release.keyLow = 0U;
    release.keyHigh = 127U;
    release.velocityLow = 1U;
    release.velocityHigh = 127U;
    release.rootNote = 72U;
    release.gain = 0.54F;
    release.startFrame = 0U;
    release.endFrame = kSourceFrames;
    map.contentHash = map.calculate_content_hash();
    return map;
}

SynthPreset make_preset() {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "DVE Production Granular Glass Choir";
    preset.metadata.author = "DVE Audio";
    preset.metadata.category = "Granular";
    preset.metadata.version = "1.27";
    preset.metadata.tags[0] = "granular";
    preset.metadata.tags[1] = "sample-map";
    preset.metadata.tags[2] = "ambient";
    preset.metadata.tagCount = 3;
    preset.sampleBank = {};
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;

    auto& granular = preset.oscillators[0];
    granular.enabled = true;
    granular.waveform = OscillatorWaveform::Granular;
    granular.gain = 0.75F;
    granular.grainPosition = 0.26F;
    granular.grainSizeMilliseconds = 92.0F;
    granular.grainDensityHertz = 27.0F;
    granular.grainSpray = 0.22F;
    granular.grainPitchSemitones = 0.0F;
    granular.grainPitchRandomSemitones = 7.0F;
    granular.grainPitchQuantizeSemitones = 7.0F;
    granular.grainStereoSpread = 0.92F;
    granular.grainStereoMotion = 0.58F;
    granular.grainEnvelopeCurve = 1.55F;
    granular.grainReverseProbability = 0.12F;
    granular.grainDensityVelocity = 0.30F;
    granular.grainDensityTimbre = 0.22F;
    granular.grainWindow = GrainWindow::Hann;

    auto& sample = preset.oscillators[1];
    sample.enabled = true;
    sample.waveform = OscillatorWaveform::Sample;
    sample.gain = 0.30F;
    sample.semitones = -12.0F;
    sample.sampleLoop = true;
    sample.sampleStart = 0.02F;
    sample.sampleEnd = 0.96F;
    sample.pan = -0.16F;

    preset.ampEnvelope = {0.035F, 0.24F, 0.72F, 0.8F, EnvelopeCurve::Exponential, 0.0F, 0.02F};
    preset.filter.topology = FilterTopology::OberheimSem;
    preset.filter.cutoffHertz = 6200.0F;
    preset.filter.resonance = 0.22F;
    preset.filter.morph = 0.18F;
    preset.filter.envelopeAmountOctaves = 1.1F;
    preset.chorus.enabled = true;
    preset.chorus.mix = 0.22F;
    preset.reverb.enabled = true;
    preset.reverb.mix = 0.32F;
    preset.delay.enabled = true;
    preset.delay.mix = 0.10F;
    preset.masterGain = 0.85F;
    return preset;
}
} // namespace

int main(int argc, char** argv) {
    using namespace dve::audio;
    const std::filesystem::path wav = argc > 1 ? argv[1] : "dve_sample_granular_demo.wav";
    const std::filesystem::path presetPath = argc > 2 ? argv[2] : "dve_sample_granular_demo.dvesynth";
    const std::filesystem::path reportPath = argc > 3 ? argv[3] : "dve_sample_granular_demo.json";
    const std::filesystem::path mapPath = argc > 4 ? argv[4] : "dve_sample_granular_demo.dvesamplemap";

    SynthPreset preset = make_preset();
    SynthSampleMap map = make_sample_map();
    std::string error;
    if (!preset.save(presetPath, &error)) { std::cerr << error << '\n'; return 1; }
    if (!save_sample_map(mapPath, map, &error)) { std::cerr << error << '\n'; return 1; }

    Synthesizer synth(48000U);
    synth.set_preset(preset);
    if (!synth.set_sample_map(map, &error)) { std::cerr << error << '\n'; return 1; }

    constexpr std::size_t frames = 48000U * 6U;
    std::vector<float> audio(frames * 2U);
    const std::array<std::uint8_t, 6> notes{48, 55, 60, 64, 67, 72};
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const std::uint64_t on = static_cast<std::uint64_t>(i) * 24000U;
        const std::uint64_t off = on + 72000U;
        (void)synth.note_on(notes[i], 0.62F + 0.05F * static_cast<float>(i), 0U, on);
        (void)synth.note_off(notes[i], 0.0F, 0U, off);
    }
    synth.render(audio);
    if (!write_float_wav(wav, audio, 48000U, &error)) { std::cerr << error << '\n'; return 1; }

    double energy = 0.0;
    float peak = 0.0F;
    for (const float value : audio) {
        energy += static_cast<double>(value) * value;
        peak = std::max(peak, std::abs(value));
    }
    const double rms = std::sqrt(energy / static_cast<double>(audio.size()));
    const SynthGranularProfiler profiler = synth.granular_profiler();
    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    if (!report) { std::cerr << "could not create demo report\n"; return 1; }
    report << "{\n"
           << "  \"preset\": \"" << preset.name << "\",\n"
           << "  \"sample_map\": \"" << map.name << "\",\n"
           << "  \"sample_map_hash\": " << map.calculate_content_hash() << ",\n"
           << "  \"sources\": " << map.sourceCount << ",\n"
           << "  \"zones\": " << map.zoneCount << ",\n"
           << "  \"resident_frames\": " << map.residentFrameCount << ",\n"
           << "  \"grain_capacity_per_oscillator\": " << kSynthGrainsPerOscillator << ",\n"
           << "  \"duration_seconds\": 6.0,\n"
           << "  \"peak\": " << peak << ",\n"
           << "  \"rms\": " << rms << ",\n"
           << "  \"requested_grains\": " << profiler.requestedGrains << ",\n"
           << "  \"admitted_grains\": " << profiler.admittedGrains << ",\n"
           << "  \"grain_steals\": " << profiler.grainSteals << ",\n"
           << "  \"grain_misses\": " << profiler.grainMisses << ",\n"
           << "  \"page_underruns\": " << profiler.pageUnderruns << ",\n"
           << "  \"maximum_active_grains\": " << profiler.maximumActiveGrains << "\n"
           << "}\n";
    std::cout << "wrote " << wav << ", " << presetPath << ", " << mapPath << " and " << reportPath << '\n';
    return 0;
}
