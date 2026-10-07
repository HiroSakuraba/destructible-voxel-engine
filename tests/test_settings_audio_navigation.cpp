#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "dve/editor_native.hpp"
#include "dve/editor_platform_bridge.hpp"

using namespace dve;
using namespace dve::editor;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void set(EditorSettingsRegistry& settings, std::string_view id, SettingValue value) {
    std::string error;
    if (!settings.set(SettingScope::Session, id, std::move(value), &error)) throw std::runtime_error(error);
}
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dve-audio-navigation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void startup_and_streams() {
    TempDirectory files;
    auto saved = EditorSettingsRegistry::make_default();
    std::string error;
    require(saved.set(SettingScope::User, "audio.sample_rate", std::int64_t{96000}, &error), "rate not saved");
    require(saved.set(SettingScope::User, "audio.stream_preload_ms", std::int64_t{100}, &error), "preload not saved");
    const auto prefs = files.path / "settings.txt";
    require(saved.save_scope_file(SettingScope::User, prefs, &error), "preferences file failed");
    NativeEditorController controller{EditorWorkspace{}, prefs};
    auto& mixer = controller.audio_mixer();
    require(mixer.sample_rate() == 96000 && controller.synthesizer().sample_rate() == 96000,
            "persisted rate loaded after engine creation");
    require(mixer.stream_preload_frames() == 9600, "persisted stream preload ignored engine rate");
    set(controller.workspace().settings(), "audio.sample_rate", std::int64_t{8000});
    controller.update(0.01F);
    require(mixer.sample_rate() == 96000, "restart preference recreated the active engine");
    set(controller.workspace().settings(), "audio.stream_preload_ms", std::int64_t{50});
    controller.update(0.01F);
    audio::DecodedAudioAsset asset;
    asset.metadata.sampleRate = mixer.sample_rate();
    asset.metadata.channels = 1;
    asset.metadata.frameCount = 24000;
    asset.metadata.storagePolicy = audio::AudioStoragePolicy::Streamed;
    asset.samples.resize(24000, 0.25F);
    const auto file = files.path / "stream.dvesample";
    require(audio::write_cooked_audio_asset(file, asset, &error), "stream fixture failed");
    const auto first = mixer.register_streamed_sample(file, 0, &error);
    if (!first) throw std::runtime_error("stream registration failed: " + error);
    if (mixer.stream_capacity_frames(first) != 4800)
        throw std::runtime_error("new stream ring ignored preference: " + std::to_string(mixer.stream_capacity_frames(first)));
    set(controller.workspace().settings(), "audio.stream_preload_ms", std::int64_t{250});
    controller.update(0.01F);
    const auto second = mixer.register_streamed_sample(file, 0, &error);
    require(static_cast<bool>(second) && mixer.stream_capacity_frames(second) == 24000, "changed preload not used at open");
    require(mixer.stream_capacity_frames(first) == 4800, "live preference resized an existing ring");
    const auto explicitRing = mixer.register_streamed_sample(file, 4096, &error);
    require(mixer.stream_capacity_frames(explicitRing) == 4096, "explicit caller capacity was overwritten");
    set(controller.workspace().settings(), "audio.stream_preload_ms", std::int64_t{0});
    controller.update(0.01F);
    const auto minimal = mixer.register_streamed_sample(file, 0, &error);
    require(mixer.stream_capacity_frames(minimal) == 1024, "zero preload lost safe ring minimum");
    audio::AudioMixer unusualRate(44100);
    unusualRate.set_stream_preload_milliseconds(33);
    require(unusualRate.stream_preload_frames() == 1456, "fractional preload frames rounded down");
    audio::AudioMixer maximumRate(384000);
    require(maximumRate.sample_rate() == maximumRate.synthesizer().sample_rate(), "maximum rate split mixer and synth clocks");
    std::vector<float> output(512);
    require(maximumRate.synthesizer().note_on(60, 0.5F), "high-rate note failed");
    maximumRate.render(output);
    require(std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x); }), "high-rate render not finite");
    EditorWorkspace session;
    set(session.settings(), "audio.sample_rate", std::int64_t{8000});
    NativeEditorController layered{std::move(session), prefs};
    require(layered.audio_mixer().sample_rate() == 8000, "startup discarded session precedence");
    const auto projectPrefs = files.path / "project" / "settings.txt";
    {
        NativeEditorController project{EditorWorkspace{}, prefs, projectPrefs};
        project.open_settings(SettingScope::Project, "Audio");
        require(project.settings_panel().stage(project.workspace().settings(), "audio.sample_rate",
            std::int64_t{44100}, &error), "project rate did not stage");
        require(project.settings_panel().stage(project.workspace().settings(), "audio.buffer_frames",
            std::int64_t{512}, &error), "project buffer did not stage");
        project.close_settings(true);
        require(std::filesystem::exists(projectPrefs), "project Apply did not persist preferences");
        require(project.audio_mixer().sample_rate() == 96000, "project Apply ignored restart lifecycle");
    }
    NativeEditorController reopened{EditorWorkspace{}, prefs, projectPrefs};
    require(reopened.audio_mixer().sample_rate() == 44100, "project rate did not survive restart");
    require(std::get<std::int64_t>(reopened.workspace().settings().value("audio.buffer_frames")) == 512,
            "project buffer did not survive restart");
}
void quality_and_presets() {
    using namespace audio;
    std::vector<float> source(8192, 0.2F);
    GranularSource bank{source.data(), static_cast<std::uint32_t>(source.size()), 48000, 60};
    GranularParameters parameters;
    parameters.densityHz = 4000;
    parameters.durationMs = 500;
    parameters.cloud01 = 0.25F;
    for (auto quality : {GranularRuntimeQuality::Low, GranularRuntimeQuality::Medium,
                         GranularRuntimeQuality::High, GranularRuntimeQuality::Ultra}) {
        GranularEngine engine;
        for (int i = 0; i < 1200; ++i) {
            const auto [l, r] = engine.render(bank, parameters, 0, 261.625565F, quality);
            require(std::isfinite(l) && std::isfinite(r), "quality render not finite");
        }
        const auto limit = quality == GranularRuntimeQuality::Low ? 16U
            : quality == GranularRuntimeQuality::Medium ? 32U : 64U;
        require(engine.active_grain_count() == limit, "runtime grain admission budget ignored");
        const auto interpolation = quality == GranularRuntimeQuality::Low ? GrainInterpolation::Linear
            : quality == GranularRuntimeQuality::Ultra ? GrainInterpolation::Sinc8 : GrainInterpolation::Cubic;
        for (const auto& grain : engine.grains()) if (grain.active)
            require(grain.interpolation == interpolation, "runtime interpolation not used");
        (void)engine.render(bank, parameters, 0, 261.625565F, GranularRuntimeQuality::Low);
        require(engine.active_grain_count() == limit, "quality change killed sounding grains");
    }
    for (int i = 0; i < 100; ++i)
        require(std::fabs(GranularEngine::interpolated_sample(source.data(), 8192, i * 0.71F,
            GrainInterpolation::Sinc8, 2) - 0.2F) < 1e-6F, "sinc did not preserve DC at edges");
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = i % 2 == 0 ? 1.0F : -1.0F;
    require(std::fabs(GranularEngine::interpolated_sample(source.data(), 8192, 200,
        GrainInterpolation::Sinc8, 2)) < 0.1F, "ultra did not attenuate a source above playback Nyquist");

    NativeEditorController controller;
    auto preset = SynthPreset::make_default();
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].waveform = OscillatorWaveform::Granular;
    preset.sampleBank.enabled = true;
    preset.sampleBank.sampleRate = 48000;
    preset.sampleBank.frameCount = 8192;
    std::fill_n(preset.sampleBank.samples.begin(), 8192, 0.2F);
    preset.granular = parameters;
    preset.granular.granularQuality = FilterQuality::Offline;
    auto& synth = controller.synthesizer();
    synth.set_preset(preset);
    set(controller.workspace().settings(), "audio.granular_quality", std::string("low"));
    controller.update(0.01F);
    require(synth.note_on(60, 0.8F), "granular note failed");
    std::vector<float> output(2400);
    synth.render(output);
    require(synth.granular_profiler().grainMisses > 0, "editor low policy did not throttle actual synth");
    require(synth.preset().granular.granularQuality == FilterQuality::Offline, "preference rewrote authored quality");
    synth.set_preset(preset);
    require(synth.granular_runtime_quality() == GranularRuntimeQuality::Low, "preset load erased runtime quality");
    for (const auto* id : {"medium", "high", "ultra"}) {
        set(controller.workspace().settings(), "audio.granular_quality", std::string(id));
        controller.update(0.01F);
        const auto expected = std::string_view(id) == "medium" ? GranularRuntimeQuality::Medium
            : std::string_view(id) == "high" ? GranularRuntimeQuality::High : GranularRuntimeQuality::Ultra;
        require(synth.granular_runtime_quality() == expected, "quality choice did not reach synth");
    }
}
void begin_fly(NativeEditorController& controller) {
    const auto viewport = controller.layout().viewport;
    controller.pointer_down(PointerButton::Secondary, viewport.x + 100, viewport.y + 100);
    controller.key_down("W", false, false, false);
}
void navigation() {
    for (const auto times : {std::array<double, 2>{0, 0}, {0.3, 0}, {0, 0.2}, {0.2, 0.2}, {0.3, 0.2}, {0.2, 0.200001}}) {
        CameraNavigationFilter whole, split;
        const auto distance = whole.integrate({1, 2, 3}, 1, times[0], times[1]);
        Float3 sum{};
        for (int i = 0; i < 120; ++i) sum = add(sum, split.integrate({1, 2, 3}, 1.0 / 120, times[0], times[1]));
        require(length_squared(subtract(sum, distance)) < 1e-8F, "navigation displacement depends on frame subdivision");
    }
    NativeEditorController instant, delayed;
    set(delayed.workspace().settings(), "camera.input_acceleration", 0.3);
    set(delayed.workspace().settings(), "camera.input_smoothing", 0.2);
    delayed.update(0.01F);
    const auto instantStart = instant.camera().position, delayedStart = delayed.camera().position;
    begin_fly(instant); begin_fly(delayed);
    require(length_squared(subtract(delayed.camera().position, delayedStart)) == 0, "key-down bypassed acceleration");
    instant.update(0.1F); delayed.update(0.1F);
    const float directDistance = length_squared(subtract(instant.camera().position, instantStart));
    const float filteredDistance = length_squared(subtract(delayed.camera().position, delayedStart));
    require(filteredDistance > 0 && filteredDistance < directDistance * 0.05F, "camera acceleration/smoothing ineffective");
    delayed.key_up("W", false, false, false);
    const auto coasting = delayed.camera().position;
    delayed.update(0.1F);
    require(length_squared(subtract(coasting, delayed.camera().position)) > 0,
            "filtered movement stopped abruptly when no key was held");
    delayed.clear_navigation_input();
    const auto stopped = delayed.camera().position;
    delayed.update(1);
    require(length_squared(subtract(stopped, delayed.camera().position)) == 0, "capture loss left movement drifting");
    begin_fly(delayed); delayed.update(0.1F);
    delayed.set_camera_mode(camera::CameraRigMode::FreeFly);
    delayed.key_up("W", false, false, false);
    const auto switched = delayed.camera().position;
    delayed.update(0.1F);
    require(length_squared(subtract(switched, delayed.camera().position)) == 0, "mode change retained filtered velocity");
    delayed.pointer_up(PointerButton::Secondary, 400, 200);

    NativeEditorController look;
    set(look.workspace().settings(), "camera.input_smoothing", 0.2);
    look.update(0.01F);
    const auto viewport = look.layout().viewport;
    const int x = viewport.x + 100, y = viewport.y + 100;
    look.pointer_down(PointerButton::Secondary, x, y);
    const auto before = look.camera().target;
    look.pointer_move(x + 50, y);
    require(length_squared(subtract(before, look.camera().target)) == 0, "mouse smoothing applied an immediate jump");
    look.update(0.1F);
    require(length_squared(subtract(before, look.camera().target)) > 0, "mouse smoothing lost input");
    look.pointer_up(PointerButton::Secondary, x + 50, y);
    const auto released = look.camera().target;
    look.update(1);
    require(length_squared(subtract(released, look.camera().target)) == 0, "mouse release left residual look");
    begin_fly(look); look.update(0.1F);
    EditorPlatformBridge bridge(look);
    platform::PlatformEvent focus;
    focus.type = platform::EventType::WindowFocusLost;
    bridge.handle_event(focus);
    const auto lost = look.camera().position;
    bridge.update(1);
    require(length_squared(subtract(lost, look.camera().position)) == 0, "focus loss left filtered input");
    set(look.workspace().settings(), "camera.input_smoothing", 0.0);
    look.update(0.01F);
    look.pointer_down(PointerButton::Secondary, x, y);
    const auto directLook = look.camera().target;
    look.pointer_move(x + 20, y);
    require(length_squared(subtract(directLook, look.camera().target)) > 0, "zero smoothing did not restore immediate look");
}
} // namespace
int main() {
    try {
        startup_and_streams(); quality_and_presets(); navigation();
        std::cout << "dve_settings_audio_navigation_tests: PASS\n";
    } catch (const std::exception& error) {
        std::cerr << "dve_settings_audio_navigation_tests: FAIL: " << error.what() << '\n'; return 1;
    }
}
