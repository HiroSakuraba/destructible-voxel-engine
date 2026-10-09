// Batch coverage for the audio settings wiring: audio.spatializer
// (native / steam_audio / none) resolved by editor::audio_spatializer_mode
// and instantiated by editor::make_audio_spatializer for the editor mixer.
#include "dve/editor_runtime_settings.hpp"
#include "dve/audio/spatializer.hpp"
#include "dve/audio/steam_audio.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float eps = 1.0e-5F) { return std::abs(a - b) <= eps; }

void set(EditorSettingsRegistry& settings, std::string_view id, SettingValue value) {
    std::string error;
    if (!settings.set(SettingScope::Session, id, std::move(value), &error))
        throw std::runtime_error(error);
}

// An emitter far off the listener's left, where the analytic spatializer pans
// hard and attenuates heavily -- the case where "none" must differ most.
audio::AudioListenerState listener() { return {}; }
audio::AudioEmitterState far_left_emitter() {
    audio::AudioEmitterState emitter;
    emitter.position = {-30.0F, 0.0F, 0.0F};
    emitter.minDistanceMeters = 1.0F;
    emitter.maxDistanceMeters = 40.0F;
    return emitter;
}

void test_mode_resolver() {
    auto registry = EditorSettingsRegistry::make_default();
    require(audio_spatializer_mode(registry) == AudioSpatializerMode::Native,
            "default spatializer mode changed");
    set(registry, "audio.spatializer", SettingValue{std::string{"steam_audio"}});
    require(audio_spatializer_mode(registry) == AudioSpatializerMode::SteamAudio,
            "steam_audio mode not resolved");
    set(registry, "audio.spatializer", SettingValue{std::string{"none"}});
    require(audio_spatializer_mode(registry) == AudioSpatializerMode::PassThrough,
            "none mode not resolved");
    set(registry, "audio.spatializer", SettingValue{std::string{"native"}});
    require(audio_spatializer_mode(registry) == AudioSpatializerMode::Native,
            "native mode not resolved");
}

void test_pass_through() {
    const audio::PassThroughSpatializer pass;
    const auto result = pass.spatialize(listener(), far_left_emitter());
    require(near(result.leftGain, 0.70710678F) && near(result.rightGain, 0.70710678F),
            "pass-through must stay centered");
    require(near(result.distanceGain, 1.0F), "pass-through must not attenuate with distance");
    require(near(result.dopplerRatio, 1.0F), "pass-through must not apply doppler");
    require(near(result.lowPassHertz, 20000.0F), "pass-through must not low-pass");
    require(near(result.reverbSend, 0.0F), "pass-through must not add reverb send");
    require(near(result.propagationDelaySeconds, 0.0F), "pass-through must not delay");

    const audio::AnalyticSpatializer analytic;
    const auto contrast = analytic.spatialize(listener(), far_left_emitter());
    require(!near(contrast.leftGain, contrast.rightGain, 1.0e-3F),
            "analytic contrast case must actually pan");
    require(contrast.distanceGain < 0.5F, "analytic contrast case must actually attenuate");
}

void test_factory() {
    auto registry = EditorSettingsRegistry::make_default();
    require(make_audio_spatializer(registry) == nullptr,
            "native mode must reset the mixer to its analytic default");

    set(registry, "audio.spatializer", SettingValue{std::string{"none"}});
    const auto none = make_audio_spatializer(registry);
    require(none != nullptr, "none mode must produce a spatializer");
    const auto none_result = none->spatialize(listener(), far_left_emitter());
    require(near(none_result.distanceGain, 1.0F) && near(none_result.leftGain, none_result.rightGain),
            "none mode spatializer must pass through");

    set(registry, "audio.spatializer", SettingValue{std::string{"steam_audio"}});
    const auto steam = make_audio_spatializer(registry);
    require(steam != nullptr, "steam_audio mode must produce a spatializer");
    const auto* steam_typed = dynamic_cast<const audio::SteamAudioSpatializer*>(steam.get());
    require(steam_typed != nullptr, "steam_audio mode must produce a SteamAudioSpatializer");
    // No native backend is linked into the editor: the spatializer must serve
    // its analytic fallback and count it, never pretend a backend answered.
    const auto steam_result = steam->spatialize(listener(), far_left_emitter());
    const auto metrics = steam_typed->metrics();
    require(metrics.fallbackQueries == 1 && metrics.backendQueries == 0,
            "backend-less SteamAudioSpatializer must count fallback queries only");
    const audio::AnalyticSpatializer analytic;
    const auto analytic_result = analytic.spatialize(listener(), far_left_emitter());
    require(near(steam_result.leftGain, analytic_result.leftGain) &&
            near(steam_result.rightGain, analytic_result.rightGain) &&
            near(steam_result.distanceGain, analytic_result.distanceGain),
            "steam fallback output must match the analytic spatializer");
}

} // namespace

int main() {
    try {
        test_mode_resolver();
        test_pass_through();
        test_factory();
    } catch (const std::exception& error) {
        std::cerr << "settings audio batch test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "settings audio batch tests passed\n";
    return 0;
}
