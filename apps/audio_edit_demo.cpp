#include "dve/audio/audio_analysis.hpp"
#include "dve/audio/audio_edit.hpp"
#include "dve/audio/audio_recorder.hpp"
#include "dve/audio/audio_recovery.hpp"
#include "dve/audio/interactive_music.hpp"
#include "dve/audio/mixer.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

bool write_analysis_json(const std::filesystem::path& path,
                         const dve::audio::AudioAnalysisReport& report,
                         const dve::audio::AudioWaveformPeakCache& peaks,
                         const dve::audio::AudioSpectrogram& spectrogram,
                         const dve::audio::InteractiveMusicDecision& decision,
                         std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "unable to open analysis report";
        return false;
    }
    stream << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"sample_peak\": " << report.samplePeak << ",\n"
           << "  \"approximate_true_peak\": " << report.approximateTruePeak << ",\n"
           << "  \"rms\": " << report.rms << ",\n"
           << "  \"dc_offset\": " << report.dcOffset << ",\n"
           << "  \"crest_factor\": " << report.crestFactor << ",\n"
           << "  \"approximate_integrated_lufs\": " << report.approximateIntegratedLufs << ",\n"
           << "  \"clipped_samples\": " << report.clippedSampleCount << ",\n"
           << "  \"silent_frames\": " << report.silentFrameCount << ",\n"
           << "  \"transients\": " << report.transientFrames.size() << ",\n"
           << "  \"estimated_tempo_bpm\": " << report.estimatedTempoBpm << ",\n"
           << "  \"tempo_confidence\": " << report.tempoConfidence << ",\n"
           << "  \"waveform_levels\": " << peaks.levels.size() << ",\n"
           << "  \"spectrogram_time_bins\": " << spectrogram.timeBins << ",\n"
           << "  \"spectrogram_frequency_bins\": " << spectrogram.frequencyBins << ",\n"
           << "  \"interactive_transition_frame\": " << decision.transitionFrame << ",\n"
           << "  \"interactive_crossfade_frames\": " << decision.crossfadeFrames << "\n"
           << "}\n";
    if (!stream) {
        if (error) *error = "unable to write analysis report";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace dve::audio;
    const std::filesystem::path outputDirectory = argc > 1 ? argv[1] : "audio_edit_demo_output";
    std::filesystem::create_directories(outputDirectory);

    AudioMixer mixer(48000U);
    auto dryRecorder = std::make_shared<AudioTakeRecorder>(48000U, 2U, 65536U);
    auto masterRecorder = std::make_shared<AudioTakeRecorder>(48000U, 2U, 65536U);
    if (!dryRecorder->start("Eightfold dry synth take") ||
        !masterRecorder->start("Eightfold master take") ||
        !mixer.set_capture_sink(0U, dryRecorder, AudioCaptureTap::SynthDry) ||
        !mixer.set_capture_sink(1U, masterRecorder, AudioCaptureTap::MasterPost)) {
        std::cerr << "unable to start simultaneous recorders\n";
        return 1;
    }

    constexpr std::array<std::uint8_t, 12> notes{48, 55, 60, 63, 67, 72, 75, 79, 72, 67, 63, 60};
    std::array<float, 512> output{};
    for (std::size_t step = 0; step < notes.size(); ++step) {
        const std::uint64_t start = mixer.current_frame();
        (void)mixer.synthesizer().note_on(notes[step], 0.62F + 0.02F * static_cast<float>(step % 4U), 0U, start);
        for (int block = 0; block < 20; ++block) mixer.render(output);
        (void)mixer.synthesizer().note_off(notes[step], 0.0F, 0U, mixer.current_frame());
        for (int block = 0; block < 6; ++block) mixer.render(output);
    }
    for (int block = 0; block < 24; ++block) mixer.render(output);

    std::string error;
    auto dryTake = dryRecorder->stop(&error);
    auto masterTake = masterRecorder->stop(&error);
    mixer.clear_capture_sinks();
    if (!dryTake || !masterTake) {
        std::cerr << (error.empty() ? "unable to finish recorded takes" : error) << '\n';
        return 1;
    }

    const auto dryWav = outputDirectory / "recorded_synth_dry.wav";
    const auto masterWav = outputDirectory / "recorded_master.wav";
    if (!write_wav_file(dryWav, *dryTake, WavSampleEncoding::Float32, &error) ||
        !write_wav_file(masterWav, *masterTake, WavSampleEncoding::Float32, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    AudioEditSession session;
    session.name = "Eightfold v1.29 workstation remix";
    session.tempoMap = {{0U, 120.0, 4U, 4U}};
    session.markers = {{0U, "Intro"}, {48000U, "Build"}, {96000U, "Combat transition"}};
    session.loop = {48000U, 144000U, true};
    session.punch = {72000U, 120000U, 4800U, 4800U, true};

    AudioEditSourceLibrary library;
    const auto drySource = library.register_source(session, dryWav,
        std::make_shared<DecodedAudioAsset>(*dryTake), &error);
    const auto masterSource = library.register_source(session, masterWav,
        std::make_shared<DecodedAudioAsset>(*masterTake), &error);
    if (!drySource || !masterSource) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::uint64_t sourceFrames = dryTake->metadata.frameCount;
    const std::uint64_t halfFrames = sourceFrames / 2U;
    const auto musicTrack = add_audio_track(session, "Synth comp");
    const auto firstClip = add_audio_clip(session, musicTrack, drySource, 0U, 0U, halfFrames, &error);
    const auto secondClip = add_audio_clip(session, musicTrack, drySource, halfFrames, halfFrames,
                                           sourceFrames - halfFrames, &error);
    if (!firstClip || !secondClip ||
        !create_audio_crossfade(session, firstClip, secondClip, 2400U,
                                AudioEditFadeCurve::EqualPower, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    auto& music = session.tracks.front();
    music.armed = true;
    music.automation.push_back({AudioEditAutomationParameter::Gain, true,
        {{0U, 0.55F, AudioEditAutomationCurve::Linear},
         {48000U, 0.85F, AudioEditAutomationCurve::SmoothStep},
         {sourceFrames, 0.70F, AudioEditAutomationCurve::Linear}}});
    music.automation.push_back({AudioEditAutomationParameter::Pan, true,
        {{0U, -0.25F, AudioEditAutomationCurve::SmoothStep},
         {halfFrames, 0.25F, AudioEditAutomationCurve::SmoothStep},
         {sourceFrames, 0.0F, AudioEditAutomationCurve::Linear}}});
    music.sends.push_back({AudioEditSendTarget::Reverb, 0.28F, false, true});

    const auto rewindTrack = add_audio_track(session, "Rewind accent");
    const std::uint64_t excerptFrames = std::min<std::uint64_t>(sourceFrames / 3U, 48000U);
    const auto rewindClip = add_audio_clip(session, rewindTrack, masterSource,
        sourceFrames / 3U, sourceFrames - excerptFrames, excerptFrames, &error);
    if (!rewindClip) {
        std::cerr << error << '\n';
        return 1;
    }
    auto& rewind = session.tracks.back().clips.front();
    rewind.reverse = true;
    rewind.gain = 0.38F;
    rewind.pan = 0.45F;
    rewind.fadeInFrames = 1800U;
    rewind.fadeOutFrames = 2400U;
    rewind.fadeInCurve = AudioEditFadeCurve::SmoothStep;
    rewind.fadeOutCurve = AudioEditFadeCurve::EqualPower;

    const std::uint32_t alternateLane = add_audio_take_lane(session, musicTrack, &error);
    if (alternateLane == 0U || !set_audio_active_take_lane(session, musicTrack, alternateLane)) {
        std::cerr << (error.empty() ? "unable to create alternate take lane" : error) << '\n';
        return 1;
    }
    const auto alternateClip = add_audio_clip(session, musicTrack, masterSource, 0U, 0U,
                                               std::min<std::uint64_t>(sourceFrames, 48000U), &error);
    if (!alternateClip || !set_audio_active_take_lane(session, musicTrack, 0U)) {
        std::cerr << (error.empty() ? "unable to populate alternate take lane" : error) << '\n';
        return 1;
    }

    const auto sessionPath = outputDirectory / "workstation_remix_v2.dveaudioedit";
    const auto journalPath = outputDirectory / "workstation_remix_v2.dveaudiojournal";
    AudioEditRecoveryJournal journal(journalPath);
    if (!journal.append_snapshot(session, "recorded and arranged", &error) ||
        !atomic_write_audio_edit_session(sessionPath, session, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const auto bounce = bounce_audio_edit_session(session, library, "Eightfold workstation remix");
    const auto bouncePath = outputDirectory / "workstation_remix_v2.wav";
    if (!write_wav_file(bouncePath, bounce, WavSampleEncoding::Float32, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const auto peakCache = build_audio_waveform_peak_cache(bounce, 64U, 14U);
    const auto spectrogram = build_audio_spectrogram(bounce, 256U, 128U, 96U, 384U);
    const auto analysis = analyze_audio_asset(bounce);

    InteractiveMusicGraph graph;
    graph.name = "Eightfold adaptive score";
    graph.beatsPerMinute = 120.0;
    graph.beatsPerBar = 4U;
    graph.states = {
        {"Exploration", {{"Bed", bouncePath.string(), "", 0.0F, 1.0F, 0.65F, true},
                          {"Tension", bouncePath.string(), "danger", 0.25F, 0.8F, 0.55F, true}},
         {48000U, 96000U}, "", ""},
        {"Combat", {{"Combat stem", bouncePath.string(), "intensity", 0.0F, 1.0F, 1.0F, true}},
         {}, "combat_stinger.wav", ""}
    };
    graph.transitions = {{"Exploration", "Combat", InteractiveMusicQuantization::Bar, 2U,
                          "", {{"danger", InteractiveMusicComparison::GreaterEqual, 0.6F}}}};
    const std::vector<InteractiveMusicParameter> parameters{{"danger", 0.8F}, {"intensity", 0.75F}};
    const auto decision = choose_interactive_music_transition(graph, "Exploration", "Combat",
                                                               30000U, 48000U, parameters);
    if (!decision) {
        std::cerr << "unable to evaluate adaptive music transition\n";
        return 1;
    }

    const auto reportPath = outputDirectory / "workstation_analysis_v1_29.json";
    if (!write_analysis_json(reportPath, analysis, peakCache, spectrogram, *decision, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "captured dry/master stems and wrote v2 edit session, recovery journal, remix, and analysis to "
              << outputDirectory << '\n';
    return 0;
}
