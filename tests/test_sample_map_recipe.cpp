#include "dve/audio/audio_asset.hpp"
#include "dve/audio/sample_map.hpp"
#include "dve/audio/sample_map_recipe.hpp"
#include "dve/audio/synthesizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

dve::audio::DecodedAudioAsset make_asset(std::string name, float frequency, std::uint32_t frames) {
    dve::audio::DecodedAudioAsset asset;
    asset.metadata.name = std::move(name);
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 1;
    asset.samples.resize(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / 48000.0F;
        asset.samples[i] = 0.55F * std::sin(2.0F * std::numbers::pi_v<float> * frequency * t);
    }
    return asset;
}

dve::audio::SynthPreset sample_preset() {
    using namespace dve::audio;
    SynthPreset preset = SynthPreset::make_default();
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    auto& oscillator = preset.oscillators[0];
    oscillator.enabled = true;
    oscillator.waveform = OscillatorWaveform::Sample;
    oscillator.gain = 0.8F;
    oscillator.sampleLoop = true;
    oscillator.sampleOneShot = false;
    oscillator.sampleVelocityToGain = 0.0F;
    preset.ampEnvelope.attackSeconds = 0.001F;
    preset.ampEnvelope.decaySeconds = 0.001F;
    preset.ampEnvelope.sustainLevel = 1.0F;
    preset.ampEnvelope.releaseSeconds = 0.04F;
    preset.filter.enabled = false;
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    return preset;
}

double rms(const std::vector<float>& audio) {
    double sum{};
    for (float value : audio) sum += static_cast<double>(value) * value;
    return std::sqrt(sum / static_cast<double>(audio.size()));
}

} // namespace

int main() {
    using namespace dve::audio;
    const auto root = std::filesystem::temp_directory_path() / "dve_sample_map_recipe_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    std::string error;

    require(write_cooked_audio_asset(root / "low.dvesample", make_asset("Low", 220.0F, 4096), &error), error.c_str());
    require(write_cooked_audio_asset(root / "high.dvesample", make_asset("High", 440.0F, 8192), &error), error.c_str());
    require(write_cooked_audio_asset(root / "release.dvesample", make_asset("Release", 880.0F, 1024), &error), error.c_str());

    const std::string recipeText =
        "name=Merged Recipe Test\n"
        "author=DVE\n"
        "zone=low.dvesample|0|71|1|127|48|0|0.7|0|attack|forward|512|3584|128|0|0|resident|4096|8\n"
        "zone=high.dvesample|72|127|1|127|72|0|0.7|0|attack|forward|1024|7168|256|1|0|hybrid|2048|8\n"
        "zone=release.dvesample|0|127|1|127|60|0|0.25|0|release|one_shot|0|0|0|2|0|streamed|512|4\n";
    auto recipe = parse_sample_map_recipe(recipeText, &error);
    require(recipe.has_value(), error.c_str());
    require(recipe->zones.size() == 3U, "recipe parser lost zones");

    SynthSampleMap built;
    require(build_sample_map_from_recipe(*recipe, root, built, &error), error.c_str());
    require(built.sourceCount == 3U && built.zoneCount == 3U, "recipe build has wrong source/zone count");
    require(built.sources[0].preload == SamplePreloadPolicy::Resident && built.sources[0].residentFrameCount == 4096U,
            "resident policy was not embedded completely");
    require(built.sources[1].preload == SamplePreloadPolicy::Hybrid && built.sources[1].residentFrameCount == 2048U,
            "hybrid policy did not embed the requested attack prefix");
    require(built.sources[2].preload == SamplePreloadPolicy::Streamed && built.sources[2].residentFrameCount == 0U,
            "streamed policy unexpectedly embedded PCM");

    const auto recipePath = root / "instrument.recipe.txt";
    const auto mapPath = root / "instrument.dvesamplemap";
    { std::ofstream output(recipePath); output << recipeText; }
    require(cook_sample_map_recipe(recipePath, mapPath, &error), error.c_str());
    SynthSampleMap loaded;
    require(load_sample_map(mapPath, loaded, &error), error.c_str());
    require(loaded.contentHash == loaded.calculate_content_hash(), "cooked recipe map hash mismatch");

    // The merged writer uses DVESMAP2, but the loader keeps compatibility with the earlier
    // integrated runtime layout after its magic/version bytes are restored to v1.
    const auto legacyPath = root / "legacy_runtime_v1.dvesamplemap";
    std::filesystem::copy_file(mapPath, legacyPath, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream legacy(legacyPath, std::ios::in | std::ios::out | std::ios::binary);
        legacy.seekp(7); legacy.put('1');
        const char versionOne[4]{1, 0, 0, 0};
        legacy.seekp(8); legacy.write(versionOne, 4);
    }
    SynthSampleMap legacyLoaded;
    require(load_sample_map(legacyPath, legacyLoaded, &error), error.c_str());
    require(legacyLoaded.contentHash == loaded.contentHash, "legacy integrated runtime map compatibility failed");

    Synthesizer synth(48000);
    synth.set_preset(sample_preset());
    require(synth.set_sample_map(loaded, &error), error.c_str());
    require(synth.note_on(48, 1.0F), "resident recipe note-on queue failed");
    std::vector<float> audio(2048U * 2U);
    synth.render(audio);
    require(rms(audio) > 0.02, "resident recipe zone rendered silence");

    const std::string layered =
        "name=Layered\n"
        "zone=low.dvesample|0|127|1|127|60|0|1|0|attack|forward|512|3584|0|0|0|resident|4096|8\n"
        "zone=high.dvesample|0|127|1|127|60|0|1|0|attack|forward|1024|7168|0|1|0|resident|8192|8\n";
    require(!parse_sample_map_recipe(layered, &error).has_value(),
            "unsupported layered groups were silently accepted");

    std::filesystem::remove_all(root, ec);
    std::cout << "sample-map recipe tests passed\n";
    return 0;
}
