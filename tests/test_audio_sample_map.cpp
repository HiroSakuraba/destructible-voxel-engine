#include "dve/audio/sample_map.hpp"
#include "dve/audio/synthesizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dve::audio;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double rms(const std::vector<float>& samples) {
    double sum = 0.0;
    for (float sample : samples) sum += static_cast<double>(sample) * sample;
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1U, samples.size())));
}

double mean_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size() == b.size(), "audio vector size mismatch");
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(static_cast<double>(a[i] - b[i]));
    return sum / static_cast<double>(std::max<std::size_t>(1U, a.size()));
}

SynthPreset sample_preset(OscillatorWaveform waveform = OscillatorWaveform::Sample) {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "v1.27 Sample Map Test";
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    auto& oscillator = preset.oscillators[0];
    oscillator.enabled = true;
    oscillator.waveform = waveform;
    oscillator.gain = 0.75F;
    oscillator.sampleLoop = true;
    oscillator.sampleOneShot = false;
    oscillator.sampleVelocityToGain = 0.0F;
    preset.ampEnvelope.attackSeconds = 0.001F;
    preset.ampEnvelope.decaySeconds = 0.001F;
    preset.ampEnvelope.sustainLevel = 1.0F;
    preset.ampEnvelope.releaseSeconds = 0.08F;
    preset.filter.enabled = false;
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    preset.masterGain = 0.8F;
    return preset;
}

SynthSampleMap resident_round_robin_map() {
    constexpr std::uint32_t frames = 2048U;
    SynthSampleMap map;
    map.name = "Resident Round Robin and Release";
    map.sourceCount = 3U;
    map.zoneCount = 3U;
    map.residentFrameCount = frames * 3U;
    for (std::uint32_t sourceIndex = 0; sourceIndex < map.sourceCount; ++sourceIndex) {
        auto& source = map.sources[sourceIndex];
        source.name = "Source " + std::to_string(sourceIndex);
        source.preload = SamplePreloadPolicy::Resident;
        source.sampleRate = 48000U;
        source.totalFrameCount = frames;
        source.residentFrameOffset = sourceIndex * frames;
        source.residentFrameCount = frames;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float phase = static_cast<float>(i) / 48000.0F;
        map.residentSamples[i] = 0.75F * std::sin(2.0F * 3.1415926535F * 220.0F * phase);
        map.residentSamples[frames + i] = 0.60F * std::sin(2.0F * 3.1415926535F * 660.0F * phase);
        const float releaseEnvelope = 1.0F - static_cast<float>(i) / static_cast<float>(frames);
        map.residentSamples[frames * 2U + i] =
            0.8F * releaseEnvelope * ((i & 1U) == 0U ? 1.0F : -1.0F);
    }
    for (std::uint8_t index = 0; index < 2U; ++index) {
        auto& zone = map.zones[index];
        zone.sourceIndex = index;
        zone.keyLow = 0U;
        zone.keyHigh = 127U;
        zone.velocityLow = 1U;
        zone.velocityHigh = 127U;
        zone.rootNote = 57U;
        zone.startFrame = 0U;
        zone.endFrame = frames;
        zone.loopMode = SampleLoopMode::Forward;
        zone.loopStartFrame = 256U;
        zone.loopEndFrame = 1792U;
        zone.loopCrossfadeFrames = 64U;
        zone.roundRobinGroup = 1U;
        zone.roundRobinIndex = index;
        zone.roundRobinCount = 2U;
        zone.pan = index == 0U ? -0.25F : 0.25F;
    }
    auto& release = map.zones[2];
    release.trigger = SampleTrigger::Release;
    release.sourceIndex = 2U;
    release.keyLow = 0U;
    release.keyHigh = 127U;
    release.velocityLow = 1U;
    release.velocityHigh = 127U;
    release.rootNote = 60U;
    release.startFrame = 0U;
    release.endFrame = frames;
    release.gain = 0.7F;
    map.contentHash = map.calculate_content_hash();
    return map;
}

std::vector<float> render_attack(Synthesizer& synth, std::uint8_t note, std::size_t frames) {
    require(synth.note_on(note, 0.9F), "note-on queue failed");
    std::vector<float> audio(frames * 2U);
    synth.render(audio);
    return audio;
}

void silence_all(Synthesizer& synth) {
    synth.all_notes_off(true);
    std::array<float, 256> discard{};
    synth.render(discard);
}

void test_cooked_sample_map(const std::filesystem::path& root) {
    SynthSampleMap original = resident_round_robin_map();
    std::string error;
    require(original.validate(&error), error.c_str());
    const auto path = root / "production_sampler.dvesamplemap";
    require(save_sample_map(path, original, &error), error.c_str());
    SynthSampleMap loaded;
    require(load_sample_map(path, loaded, &error), error.c_str());
    require(loaded.name == original.name && loaded.sourceCount == original.sourceCount &&
            loaded.zoneCount == original.zoneCount && loaded.residentFrameCount == original.residentFrameCount &&
            loaded.contentHash == original.calculate_content_hash(),
            "cooked sample map did not round trip");
    require(loaded.zones[0].loopCrossfadeFrames == 64U && loaded.zones[1].roundRobinIndex == 1U,
            "cooked sample-map zone metadata changed");

    std::fstream corrupt(path, std::ios::in | std::ios::out | std::ios::binary);
    corrupt.seekg(-8, std::ios::end);
    char byte{};
    corrupt.read(&byte, 1);
    corrupt.seekp(-8, std::ios::end);
    byte ^= 0x20;
    corrupt.write(&byte, 1);
    corrupt.close();
    require(!load_sample_map(path, loaded, &error), "corrupted sample map passed content-hash validation");
}

void test_stream_page_cache() {
    SampleStreamCache cache;
    std::array<float, kSampleStreamPageFrames> page{};
    for (std::size_t i = 0; i < page.size(); ++i) page[i] = static_cast<float>(i) / 2048.0F;
    require(cache.publish_page(7U, 2U, 0U, page), "stream page publication failed");
    float value{};
    require(cache.read_linear(7U, 2U, 10.5F, value) && std::abs(value - 10.5F / 2048.0F) < 1.0e-6F,
            "stream page interpolation failed");
    require(!cache.read_linear(8U, 2U, 10.5F, value), "generation mismatch did not miss");
    auto metrics = cache.metrics();
    require(metrics.pagesPublished == 1U && metrics.readHits == 1U && metrics.readMisses == 1U,
            "stream cache metrics are incorrect");
}

void test_resident_zones_round_robin_and_release() {
    Synthesizer synth(48000U);
    synth.set_preset(sample_preset());
    SynthSampleMap map = resident_round_robin_map();
    std::string error;
    require(synth.set_sample_map(map, &error), error.c_str());

    const auto first = render_attack(synth, 57U, 1024U);
    silence_all(synth);
    const auto second = render_attack(synth, 57U, 1024U);
    require(rms(first) > 0.05 && rms(second) > 0.05, "resident sample-map attack rendered silence");
    require(mean_abs_difference(first, second) > 0.08,
            "round-robin zones did not alternate resident sources");

    require(synth.note_off(57U), "release note-off queue failed");
    std::vector<float> release(512U * 2U);
    synth.render(release);
    require(rms(release) > 0.03, "release-trigger zone did not render");
}

SynthSampleMap streamed_map() {
    SynthSampleMap map;
    map.name = "Streamed Window";
    map.sourceCount = 1U;
    map.zoneCount = 1U;
    auto& source = map.sources[0];
    source.name = "Streamed Source";
    source.streamPath = "audio/streamed_source.dveaudio";
    source.preload = SamplePreloadPolicy::Streamed;
    source.sampleRate = 48000U;
    source.totalFrameCount = 4096U;
    auto& zone = map.zones[0];
    zone.sourceIndex = 0U;
    zone.keyLow = 0U;
    zone.keyHigh = 127U;
    zone.velocityLow = 1U;
    zone.velocityHigh = 127U;
    zone.rootNote = 60U;
    zone.startFrame = 0U;
    zone.endFrame = 4096U;
    return map;
}


SynthSampleMap internal_loop_map() {
    SynthSampleMap map;
    map.name = "Internal Loop Boundary";
    map.sourceCount = 1U;
    map.zoneCount = 1U;
    map.residentFrameCount = 512U;
    auto& source = map.sources[0];
    source.name = "Loop Source";
    source.preload = SamplePreloadPolicy::Resident;
    source.sampleRate = 48000U;
    source.totalFrameCount = 512U;
    source.residentFrameCount = 512U;
    for (std::size_t i = 0; i < 128U; ++i) map.residentSamples[i] = 0.8F;
    auto& zone = map.zones[0];
    zone.sourceIndex = 0U;
    zone.keyLow = 0U;
    zone.keyHigh = 127U;
    zone.velocityLow = 1U;
    zone.velocityHigh = 127U;
    zone.rootNote = 60U;
    zone.startFrame = 0U;
    zone.endFrame = 512U;
    zone.loopMode = SampleLoopMode::Forward;
    zone.loopStartFrame = 64U;
    zone.loopEndFrame = 128U;
    zone.loopCrossfadeFrames = 8U;
    return map;
}

void test_internal_loop_boundary() {
    Synthesizer synth(48000U);
    synth.set_preset(sample_preset());
    std::string error;
    require(synth.set_sample_map(internal_loop_map(), &error), error.c_str());
    const auto audio = render_attack(synth, 60U, 384U);
    double lateEnergy = 0.0;
    for (std::size_t i = 256U * 2U; i < audio.size(); ++i)
        lateEnergy += static_cast<double>(audio[i]) * audio[i];
    const double lateRms = std::sqrt(lateEnergy / static_cast<double>(audio.size() - 256U * 2U));
    require(lateRms > 0.08,
            "sampler crossed an internal loop endpoint and played the zone tail instead of wrapping");
}

void test_streamed_sampler_and_underruns() {
    Synthesizer synth(48000U);
    synth.set_preset(sample_preset());
    std::string error;
    require(synth.set_sample_map(streamed_map(), &error), error.c_str());
    std::array<float, kSampleStreamPageFrames> page{};
    for (std::size_t i = 0; i < page.size(); ++i)
        page[i] = 0.7F * std::sin(2.0F * 3.1415926535F * 330.0F * static_cast<float>(i) / 48000.0F);
    require(synth.publish_sample_stream_page(0U, 0U, page), "synth stream page publication failed");
    const auto audio = render_attack(synth, 60U, 512U);
    require(rms(audio) > 0.04, "streamed sample page rendered silence");
    auto profiler = synth.granular_profiler();
    require(profiler.stream.pagesPublished == 1U && profiler.pageUnderruns == 0U,
            "streamed sampler reported an unexpected underrun");

    Synthesizer missing(48000U);
    missing.set_preset(sample_preset());
    require(missing.set_sample_map(streamed_map(), &error), error.c_str());
    const auto silent = render_attack(missing, 60U, 256U);
    require(rms(silent) < 1.0e-6, "missing stream pages did not fail closed to silence");
    require(missing.granular_profiler().pageUnderruns > 0U,
            "missing stream pages were not visible in profiler telemetry");
}

void test_granular_features_and_budget() {
    Synthesizer synth(48000U);
    SynthPreset preset = sample_preset(OscillatorWaveform::Granular);
    auto& grain = preset.oscillators[0];
    grain.grainSizeMilliseconds = 180.0F;
    grain.grainDensityHertz = 120.0F;
    grain.grainSpray = 0.35F;
    grain.grainStereoSpread = 0.8F;
    grain.grainStereoMotion = 0.75F;
    grain.grainEnvelopeCurve = 2.5F;
    grain.grainReverseProbability = 0.35F;
    grain.grainPitchRandomSemitones = 12.0F;
    grain.grainPitchQuantizeSemitones = 7.0F;
    grain.grainDensityVelocity = 0.5F;
    grain.grainDensityTimbre = 0.25F;
    synth.set_preset(preset);
    std::string error;
    require(synth.set_sample_map(resident_round_robin_map(), &error), error.c_str());
    for (std::uint8_t note = 48U; note < 64U; ++note)
        require(synth.note_on(note, 0.9F), "granular stress note-on failed");
    std::vector<float> audio(8192U * 2U);
    synth.render(audio);
    const auto profiler = synth.granular_profiler();
    require(rms(audio) > 0.005, "production granular path rendered silence");
    require(profiler.requestedGrains > 0U && profiler.admittedGrains > 0U,
            "granular profiler did not count requested/admitted grains");
    require(profiler.grainMisses > 0U,
            "voice-level grain budget did not shed background grains under overload");
    require(profiler.maximumActiveGrains <= 96U,
            "granular overload exceeded the 16-voice quality-shedding budget");

    const auto path = std::filesystem::temp_directory_path() / "v127_granular_fields.dvesynth";
    require(preset.save(path, &error), error.c_str());
    const auto loaded = SynthPreset::load(path, &error);
    require(loaded.has_value(), error.c_str());
    require(std::abs(loaded->oscillators[0].grainEnvelopeCurve - 2.5F) < 1.0e-5F &&
            std::abs(loaded->oscillators[0].grainPitchQuantizeSemitones - 7.0F) < 1.0e-5F &&
            std::abs(loaded->oscillators[0].grainStereoMotion - 0.75F) < 1.0e-5F,
            "v1.27 granular controls did not round trip through preset persistence");
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "dve_audio_sample_map_tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        test_cooked_sample_map(root);
        test_stream_page_cache();
        test_resident_zones_round_robin_and_release();
        test_internal_loop_boundary();
        test_streamed_sampler_and_underruns();
        test_granular_features_and_budget();
        std::cout << "dve_audio_sample_map_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_audio_sample_map_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
