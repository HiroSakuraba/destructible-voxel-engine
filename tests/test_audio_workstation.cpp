#include "dve/audio/audio_analysis.hpp"
#include "dve/audio/audio_edit.hpp"
#include "dve/audio/audio_recorder.hpp"
#include "dve/audio/audio_device_capture.hpp"
#include "dve/audio/audio_effects.hpp"
#include "dve/audio/audio_metering.hpp"
#include "dve/audio/audio_repair.hpp"
#include "dve/audio/audio_time_stretch.hpp"
#include "dve/audio/clap_host.hpp"
#include "dve/audio/audio_recovery.hpp"
#include "dve/audio/interactive_music.hpp"
#include "dve/audio/mixer.hpp"
#ifdef DVE_HAVE_EDITOR_TIMELINE
#include "dve/editor_audio_timeline.hpp"
#include "dve/editor_audio_workspace.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

dve::audio::DecodedAudioAsset make_rhythm(std::uint32_t frames = 192000U) {
    dve::audio::DecodedAudioAsset asset;
    asset.metadata.name = "120-bpm-rhythm";
    asset.metadata.sampleRate = 48000U;
    asset.metadata.channels = 2U;
    asset.metadata.frameCount = frames;
    asset.metadata.durationSeconds = static_cast<double>(frames) / 48000.0;
    asset.samples.assign(static_cast<std::size_t>(frames) * 2U, 0.0F);
    const std::uint32_t beatFrames = 24000U;
    for (std::uint32_t beat = 0U; beat * beatFrames < frames; ++beat) {
        const std::uint32_t start = beat * beatFrames;
        for (std::uint32_t i = 0U; i < 1000U && start + i < frames; ++i) {
            const float envelope = std::exp(-static_cast<float>(i) / 180.0F);
            const float value = envelope * std::sin(6.283185307179586F * 180.0F * static_cast<float>(i) / 48000.0F);
            asset.samples[static_cast<std::size_t>(start + i) * 2U] = value;
            asset.samples[static_cast<std::size_t>(start + i) * 2U + 1U] = value;
        }
    }
    asset.metadata.contentHash = dve::audio::audio_content_hash(asset.samples, asset.metadata);
    return asset;
}

void test_analysis() {
    using namespace dve::audio;
    const auto asset = make_rhythm();
    const auto cache = build_audio_waveform_peak_cache(asset, 64U, 12U);
    require(!cache.levels.empty() && cache.levels.front().peaks.size() > 100U,
            "waveform peak cache was not generated");
    require(choose_audio_waveform_peak_level(cache, 1024.0) != nullptr,
            "waveform peak level selection failed");
    const auto spectrogram = build_audio_spectrogram(asset, 128U, 64U, 48U, 64U);
    require(spectrogram.timeBins > 0U && spectrogram.frequencyBins == 48U &&
            spectrogram.decibels.size() == static_cast<std::size_t>(spectrogram.timeBins) * 48U,
            "spectrogram generation failed");
    const auto report = analyze_audio_asset(asset);
    require(report.samplePeak > 0.5F && report.rms > 0.01F && report.transientFrames.size() >= 4U,
            "audio analysis failed to find level/transients");
    require(report.estimatedTempoBpm >= 55.0 && report.estimatedTempoBpm <= 205.0,
            "tempo analysis returned an invalid result");
}

struct EditFixture {
    dve::audio::AudioEditSession session;
    dve::audio::AudioEditSourceLibrary library;
    dve::audio::AudioEditSourceId source{};
    dve::audio::AudioEditTrackId track{};
    dve::audio::AudioEditClipId left{};
    dve::audio::AudioEditClipId right{};
};

EditFixture make_edit_fixture(const std::filesystem::path& temp) {
    using namespace dve::audio;
    EditFixture fixture;
    auto asset = std::make_shared<DecodedAudioAsset>(make_rhythm(96000U));
    std::string error;
    fixture.source = fixture.library.register_source(fixture.session, temp / "rhythm.wav", asset, &error);
    require(static_cast<bool>(fixture.source), error.c_str());
    fixture.track = add_audio_track(fixture.session, "Music");
    fixture.left = add_audio_clip(fixture.session, fixture.track, fixture.source, 0U, 0U, 24000U, &error);
    fixture.right = add_audio_clip(fixture.session, fixture.track, fixture.source, 24000U, 24000U, 24000U, &error);
    require(static_cast<bool>(fixture.left) && static_cast<bool>(fixture.right), error.c_str());
    return fixture;
}

void test_advanced_editing_and_persistence(const std::filesystem::path& temp) {
    using namespace dve::audio;
    auto fixture = make_edit_fixture(temp);
    std::string error;
    require(create_audio_crossfade(fixture.session, fixture.left, fixture.right, 2400U,
                                   AudioEditFadeCurve::EqualPower, &error), error.c_str());
    auto& track = fixture.session.tracks.front();
    require(track.clips[1].timelineStartFrame == 21600U && track.clips[0].fadeOutFrames == 2400U,
            "crossfade did not create overlap");
    require(slip_audio_clip(fixture.session, fixture.right, 1200), "slip edit failed");
    require(track.clips[1].sourceStartFrame == 25200U, "slip edit used wrong source offset");

    track.automation.push_back({AudioEditAutomationParameter::Gain, true,
        {{0U, 0.25F, AudioEditAutomationCurve::Linear},
         {48000U, 1.0F, AudioEditAutomationCurve::SmoothStep}}});
    require(evaluate_audio_automation(track, AudioEditAutomationParameter::Gain, 24000U, 1.0F) > 0.25F,
            "gain automation interpolation failed");
    track.sends.push_back({AudioEditSendTarget::Reverb, 0.3F, false, true});
    fixture.session.markers = {{0U, "Intro"}, {24000U, "Beat 2"}};
    fixture.session.tempoMap = {{0U, 120.0, 4U, 4U}, {96000U, 90.0, 3U, 4U}};
    fixture.session.punch = {12000U, 36000U, 2400U, 2400U, true};
    require(std::abs(audio_edit_frame_to_beats(fixture.session, 48000U) - 2.0) < 0.01,
            "frame-to-beat conversion failed");
    require(audio_edit_snap_frame_to_beat(fixture.session, 25000U, 4U) == 24000U,
            "beat snapping failed");

    const std::uint32_t newLane = add_audio_take_lane(fixture.session, fixture.track, &error);
    require(newLane == 1U && set_audio_active_take_lane(fixture.session, fixture.track, newLane),
            "take lane creation/selection failed");
    const auto laneClip = add_audio_clip(fixture.session, fixture.track, fixture.source, 0U, 48000U, 12000U, &error);
    require(static_cast<bool>(laneClip) && track.clips.back().takeLane == 1U,
            "new clip was not assigned to active take lane");
    std::vector<float> activeLaneRender(14000U * 2U);
    render_audio_edit_session(fixture.session, fixture.library, 0U, activeLaneRender);
    require(std::any_of(activeLaneRender.begin(), activeLaneRender.end(),
                        [](float value) { return std::abs(value) > 1.0e-4F; }),
            "active take lane did not render");

    const auto sessionPath = temp / "advanced.dveaudioedit";
    require(set_audio_comp_segment(fixture.session, fixture.track, 0U, 12000U, 0U, 256U, 256U, &error),
            error.c_str());
    require(set_audio_comp_segment(fixture.session, fixture.track, 12000U, 12000U, 1U, 256U, 256U, &error),
            error.c_str());
    require(audio_comp_take_lane_at_frame(fixture.session.tracks.front(), 18000U) == 1U,
            "comp segment did not select the requested take lane");
    require(write_audio_edit_session(sessionPath, fixture.session, &error), error.c_str());
    const auto loaded = read_audio_edit_session(sessionPath, &error);
    require(loaded && loaded->version == 3U && loaded->markers.size() == 2U &&
            loaded->tracks.front().automation.size() == 1U && loaded->tracks.front().takeLaneCount == 2U &&
            loaded->tracks.front().compSegments.size() == 2U,
            error.empty() ? "v3 audio edit persistence failed" : error.c_str());
}

void test_recovery(const std::filesystem::path& temp) {
    using namespace dve::audio;
    auto fixture = make_edit_fixture(temp);
    const auto journalPath = temp / "recovery.dveaudiojournal";
    AudioEditRecoveryJournal journal(journalPath);
    std::string error;
    require(journal.append_snapshot(fixture.session, "initial", &error), error.c_str());
    fixture.session.name = "Recovered remix";
    fixture.session.markers.push_back({12000U, "Recovered marker"});
    require(journal.append_snapshot(fixture.session, "edited", &error), error.c_str());
    {
        std::ofstream damage(journalPath, std::ios::binary | std::ios::app);
        damage << "truncated-tail";
    }
    AudioRecoveryInfo info;
    const auto recovered = journal.recover_latest(&info, &error);
    require(recovered && recovered->name == "Recovered remix" && info.sequence == 2U &&
            info.validRecords == 2U && info.ignoredTruncatedTail,
            error.empty() ? "journal recovery failed" : error.c_str());
    const auto autosave = temp / "autosave.dveaudioedit";
    require(atomic_write_audio_edit_session(autosave, *recovered, &error), error.c_str());
    require(read_audio_edit_session(autosave, &error).has_value(), "atomic autosave was unreadable");
}

void test_interactive_music() {
    using namespace dve::audio;
    InteractiveMusicGraph graph;
    graph.name = "Combat score";
    graph.beatsPerMinute = 120.0;
    graph.beatsPerBar = 4U;
    graph.states = {
        {"Exploration", {{"Bed", "bed.flac", "", 0.0F, 1.0F, 0.8F, true},
                         {"Tension", "tension.flac", "danger", 0.25F, 0.75F, 1.0F, true}},
         {24000U, 48000U}, "", ""},
        {"Combat", {{"Drums", "drums.flac", "intensity", 0.0F, 1.0F, 1.0F, true}},
         {}, "combat-in.wav", ""}
    };
    graph.transitions = {{"Exploration", "Combat", InteractiveMusicQuantization::Bar, 2U,
                          "", {{"danger", InteractiveMusicComparison::GreaterEqual, 0.6F}}}};
    std::string error;
    require(validate_interactive_music_graph(graph, &error), error.c_str());
    const std::vector<InteractiveMusicParameter> parameters{{"danger", 0.8F}, {"intensity", 0.7F}};
    const auto stems = evaluate_interactive_music_stems(graph, "Exploration", parameters);
    require(stems.size() == 2U && stems[1].gain > 0.0F, "vertical stem evaluation failed");
    const auto decision = choose_interactive_music_transition(graph, "Exploration", "Combat",
                                                               1000U, 48000U, parameters);
    require(decision && decision->transitionFrame == 96000U && decision->crossfadeFrames == 48000U &&
            decision->stinger == "combat-in.wav", "quantized interactive transition failed");
    graph.states.front().containers.push_back({"Footsteps", InteractiveMusicContainerMode::Sequence, true,
        {{"Step A", "step-a.wav", 1.0F, 1.0F, 0U}, {"Step B", "step-b.wav", 1.0F, 0.8F, 0U}}});
    const auto path = std::filesystem::temp_directory_path() / "dve_interactive_music_test.dveinteractiveaudio";
    require(write_interactive_music_graph(path, graph, &error), error.c_str());
    const auto loaded = read_interactive_music_graph(path, &error);
    require(loaded && loaded->states.front().containers.size() == 1U &&
            interactive_music_graph_hash(*loaded) == interactive_music_graph_hash(graph),
            error.empty() ? "interactive music graph persistence failed" : error.c_str());
    InteractiveMusicRuntime runtime(*loaded, 7U);
    const auto first = runtime.trigger_container("Footsteps", 0U);
    const auto second = runtime.trigger_container("Footsteps", 1U);
    require(first && second && first->variant == "Step A" && second->variant == "Step B",
            "interactive music sequence container failed");
    std::filesystem::remove(path);
}

void test_metering_repair_effects_and_clap(const std::filesystem::path& temp) {
    using namespace dve::audio;
    auto asset = make_rhythm(96000U);
    for (float& sample : asset.samples) sample += 0.02F;
    asset.metadata.contentHash = audio_content_hash(asset.samples, asset.metadata);
    const auto loudness = measure_audio_loudness_ebu_r128(asset, true);
    require(loudness.ebuR128Compatible && loudness.gatedBlockCount > 0U &&
            std::isfinite(loudness.integratedLufs) && loudness.truePeak >= loudness.samplePeak * 0.9F,
            "EBU-style loudness analysis failed");
    const auto repaired = audio_remove_dc(asset);
    const auto report = analyze_audio_asset(repaired);
    require(std::abs(report.dcOffset) < 0.002F, "DC removal failed");
    const auto normalized = audio_normalize_loudness(repaired, -18.0F, 0.95F);
    const auto stretched = audio_time_stretch_wsola(repaired, 1.25);
    require(stretched.metadata.frameCount == 120000U &&
            std::all_of(stretched.samples.begin(), stretched.samples.end(), [](float value) { return std::isfinite(value); }),
            "WSOLA time stretch failed");
    const auto shifted = audio_pitch_shift_wsola(repaired, 3.0);
    require(shifted.metadata.frameCount == repaired.metadata.frameCount && !shifted.samples.empty(),
            "pitch shift duration restoration failed");
    const std::array<AudioWarpMarker, 3> warp{{{0U, 0U}, {48000U, 60000U}, {96000U, 108000U}}};
    const auto warped = audio_render_warp_markers(repaired, warp);
    require(warped.metadata.frameCount == 108000U, "piecewise warp-marker render failed");
    const auto normalizedMeter = measure_audio_loudness_ebu_r128(normalized);
    require(normalizedMeter.truePeak <= 0.951F && normalized.metadata.frameCount == repaired.metadata.frameCount,
            "loudness normalization violated peak/frame constraints");

    GainAudioEffect gain(0.5F);
    std::array<float, 8> deterministicInput{0.1F, -0.1F, 0.3F, -0.3F, 0.5F, -0.5F, 0.7F, -0.7F};
    require(deterministic_effect_render_check(gain, deterministicInput, 48000U, 2U),
            "effect determinism check failed");
    AudioEffectRack rack(48000U, 2U, 16U);
    require(rack.add(std::make_unique<GainAudioEffect>(0.5F), 1.0F), "effect rack insert failed");
    auto processed = deterministicInput;
    rack.process(processed);
    require(std::abs(processed[0] - 0.05F) < 1.0e-6F && rack.state().size() == 1U,
            "effect rack processing/state failed");
    require(rack.add_missing({"missing.example", 1U, 1.0F, false, true, {}}),
            "missing plugin placeholder failed");

    const auto pluginDir = temp / "clap";
    std::filesystem::create_directories(pluginDir);
    const auto pluginPath = pluginDir / "test-effect.clap";
    { std::ofstream out(pluginPath, std::ios::binary); out << "fake-clap-binary-v1"; }
    ClapPluginRegistry registry;
    registry.add_search_path(pluginDir);
    require(registry.scan() == 1U && registry.plugins().front().status == ClapScanStatus::Available,
            "CLAP discovery/hash scan failed");
    const auto cachePath = temp / "plugins.dveclapcache";
    std::string error;
    require(registry.write_cache(cachePath, &error), error.c_str());
    ClapPluginRegistry loaded;
    require(loaded.read_cache(cachePath, &error) && loaded.plugins().size() == 1U, error.c_str());
}

void test_capture_clock_reconciliation() {
    using namespace dve::audio;
    AudioCaptureClockReconciler reconciler(44100U, 48000U, 1U, 8192U);
    std::vector<float> input(4410U);
    for (std::size_t i = 0U; i < input.size(); ++i)
        input[i] = std::sin(6.283185307179586 * 440.0 * static_cast<double>(i) / 44100.0);
    require(reconciler.push_device_frames(input) == input.size(), "capture clock input ring rejected valid frames");
    std::vector<float> output(4600U);
    const auto produced = reconciler.drain_engine_frames(output);
    const auto telemetry = reconciler.telemetry();
    require(produced > 4300U && produced <= output.size() && telemetry.engineFramesDelivered == produced &&
            telemetry.nominalRatio > 0.9 && telemetry.nominalRatio < 1.0,
            "capture clock reconciliation failed");
}

void test_capture_fanout() {
    using namespace dve::audio;
    AudioMixer mixer(48000U);
    auto synthRecorder = std::make_shared<AudioTakeRecorder>(48000U, 2U, 32768U);
    auto masterRecorder = std::make_shared<AudioTakeRecorder>(48000U, 2U, 32768U);
    require(synthRecorder->start("synth") && masterRecorder->start("master"),
            "fan-out recorders did not start");
    require(mixer.set_capture_sink(0U, synthRecorder, AudioCaptureTap::SynthDry) &&
            mixer.set_capture_sink(1U, masterRecorder, AudioCaptureTap::MasterPost),
            "capture fan-out registration failed");
    const auto start = mixer.current_frame();
    require(mixer.synthesizer().note_on(60U, 0.8F, 0U, start), "fan-out synth note failed");
    std::array<float, 512> block{};
    for (int i = 0; i < 40; ++i) mixer.render(block);
    std::string error;
    auto synth = synthRecorder->stop(&error);
    auto master = masterRecorder->stop(&error);
    mixer.clear_capture_sinks();
    require(synth && master && synth->metadata.frameCount == master->metadata.frameCount &&
            synth->metadata.frameCount > 5000U && synth->metadata.peakLinear > 0.01F &&
            master->metadata.peakLinear > 0.01F,
            "simultaneous master/stem capture failed");
}

#ifdef DVE_HAVE_EDITOR_TIMELINE
void test_timeline_model(const std::filesystem::path& temp) {
    using namespace dve;
    auto fixture = make_edit_fixture(temp);
    fixture.session.markers = {{24000U, "Beat"}};
    fixture.session.loop = {0U, 48000U, true};
    fixture.session.punch = {12000U, 36000U, 0U, 0U, true};
    const auto asset = fixture.library.resolve(fixture.source);
    editor::AudioTimelinePeakCacheEntry cache{fixture.source,
        audio::build_audio_waveform_peak_cache(*asset, 64U, 12U)};
    editor::AudioTimelineView view;
    view.framesPerPixel = 120.0;
    const auto model = editor::build_audio_timeline_draw_model(
        fixture.session, view, {0, 0, 800, 240}, 24000U, {cache}, fixture.left);
    require(model.clips.size() == 2U && !model.clips.front().waveform.empty() &&
            !model.grid.empty() && model.markers.size() == 1U && model.loopRect && model.punchRect,
            "audio timeline draw model is incomplete");
    const auto hit = editor::hit_test_audio_timeline_clip(
        model, model.clips.front().rect.x + 1, model.clips.front().rect.y + 1);
    require(hit && *hit == fixture.left, "audio timeline clip hit testing failed");
    view.snap = editor::AudioTimelineSnap::Markers;
    require(editor::snap_audio_timeline_frame(fixture.session, view, 24100U, 200U) == 24000U,
            "audio timeline marker snapping failed");

    audio::AudioEditHistory history;
    audio::AudioEditTransport transport;
    editor::AudioTimelineEditorController controller(fixture.session, history, transport, view);
    controller.set_content_rect({0, 0, 800, 240});
    const std::vector<editor::AudioTimelinePeakCacheEntry> emptyCaches;
    controller.set_peak_caches(&emptyCaches);
    controller.set_tool(editor::AudioEditorTool::Move);
    const auto firstModel = controller.draw_model();
    require(!firstModel.clips.empty(), "editor controller draw model is empty");
    const auto firstRect = firstModel.clips.front().rect;
    require(controller.pointer_down(firstRect.x + firstRect.width / 2, firstRect.y + firstRect.height / 2),
            "editor move pointer down failed");
    require(controller.pointer_up(firstRect.x + firstRect.width / 2 + 20, firstRect.y + firstRect.height / 2),
            "editor move pointer up failed");
    require(fixture.session.tracks.front().clips.front().timelineStartFrame > 0U && history.can_undo(),
            "editor move/history grouping failed");
    require(controller.dispatch_command("edit.undo"), "editor command undo failed");
    controller.set_tool(editor::AudioEditorTool::Scrub);
    require(controller.pointer_down(400, 200) && transport.cursor() > 0U, "scrub tool failed");
}
#endif

} // namespace

int main() {
    const auto temp = std::filesystem::temp_directory_path() / "dve_audio_workstation_tests";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp);
    test_analysis();
    test_advanced_editing_and_persistence(temp);
    test_recovery(temp);
    test_interactive_music();
    test_metering_repair_effects_and_clap(temp);
    test_capture_clock_reconciliation();
    test_capture_fanout();
#ifdef DVE_HAVE_EDITOR_TIMELINE
    test_timeline_model(temp);
#endif
    std::filesystem::remove_all(temp);
    std::cout << "audio workstation tests passed\n";
    return 0;
}
