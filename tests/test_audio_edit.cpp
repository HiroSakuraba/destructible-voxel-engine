#include "dve/audio/audio_edit.hpp"
#include "dve/audio/audio_recorder.hpp"
#include "dve/audio/mixer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

dve::audio::DecodedAudioAsset make_tone(std::uint32_t frames = 4800U) {
    dve::audio::DecodedAudioAsset asset;
    asset.metadata.name = "edit-tone";
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 2;
    asset.metadata.frameCount = frames;
    asset.metadata.durationSeconds = static_cast<double>(frames) / 48000.0;
    asset.samples.resize(static_cast<std::size_t>(frames) * 2U);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const float value = 0.5F * std::sin(6.283185307179586F * 440.0F * static_cast<float>(frame) / 48000.0F);
        asset.samples[frame * 2U] = value;
        asset.samples[frame * 2U + 1U] = value;
    }
    asset.metadata.contentHash = dve::audio::audio_content_hash(asset.samples, asset.metadata);
    return asset;
}

void test_edit_session(const std::filesystem::path& temp) {
    using namespace dve::audio;
    AudioEditSession session;
    session.name = "Remix test";
    AudioEditSourceLibrary library;
    auto sourceAsset = std::make_shared<DecodedAudioAsset>(make_tone());
    std::string error;
    const auto source = library.register_source(session, temp / "tone.wav", sourceAsset, &error);
    require(static_cast<bool>(source), error.c_str());
    const auto track = add_audio_track(session, "Synth take");
    const auto clip = add_audio_clip(session, track, source, 120U, 100U, 2400U, &error);
    require(static_cast<bool>(clip), error.c_str());
    session.tracks.front().clips.front().gain = 0.8F;
    session.tracks.front().clips.front().pan = -0.25F;
    session.tracks.front().clips.front().fadeInFrames = 64U;
    session.tracks.front().clips.front().fadeOutFrames = 64U;

    std::vector<float> render(3000U * 2U);
    render_audio_edit_session(session, library, 0U, render);
    require(std::all_of(render.begin(), render.begin() + 200, [](float value) { return value == 0.0F; }),
            "timeline preroll was not silent");
    require(std::any_of(render.begin() + 300, render.end(), [](float value) { return std::abs(value) > 1.0e-4F; }),
            "timeline clip did not render");

    AudioEditHistory history;
    history.reset(session);
    require(move_audio_clip(session, clip, 400U), "clip move failed");
    history.commit(session);
    require(history.undo(session) && session.tracks.front().clips.front().timelineStartFrame == 120U,
            "audio edit undo failed");
    require(history.redo(session) && session.tracks.front().clips.front().timelineStartFrame == 400U,
            "audio edit redo failed");
    const auto right = split_audio_clip(session, clip, 1000U, &error);
    require(right.has_value() && session.tracks.front().clips.size() == 2U, error.c_str());

    const auto sessionPath = temp / "remix.dveaudioedit";
    require(write_audio_edit_session(sessionPath, session, &error), error.c_str());
    auto loaded = read_audio_edit_session(sessionPath, &error);
    require(loaded && loaded->tracks.size() == 1U && loaded->tracks.front().clips.size() == 2U,
            "audio edit session round trip failed");

    auto invalid = session;
    invalid.tracks.front().clips.front().source = {999999U};
    const auto invalidPath = temp / "invalid-remix.dveaudioedit";
    require(!write_audio_edit_session(invalidPath, invalid, &error),
            "invalid source reference was accepted by the writer");

    AudioEditTransport transport;
    session.loop = {400U, 900U, true};
    transport.seek(850U);
    transport.play();
    std::vector<float> transportOutput(256U * 2U);
    transport.render(session, library, transportOutput);
    require(transport.cursor() >= 400U && transport.cursor() < 900U,
            "transport loop/rewind behavior failed");
    transport.rewind();
    require(transport.cursor() == 0U, "transport rewind failed");

    const auto bounce = bounce_audio_edit_session(session, library, "remix-bounce");
    require(!bounce.samples.empty() && bounce.metadata.channels == 2U, "timeline bounce failed");
    const auto wav = temp / "remix.wav";
    require(write_wav_file(wav, bounce, WavSampleEncoding::Float32, &error), error.c_str());
    auto imported = import_wav_file(wav, {}, &error);
    require(imported && imported->metadata.frameCount == bounce.metadata.frameCount,
            "WAV bounce re-import failed");
}

void test_recorder_and_mixer_taps() {
    using namespace dve::audio;
    auto recorder = std::make_shared<AudioTakeRecorder>(48000, 2, 8192);
    require(recorder->start("manual take"), "recorder start failed");
    const auto tone = make_tone(2048U);
    recorder->capture_interleaved(tone.samples, 0U);
    std::string error;
    auto take = recorder->stop(&error);
    require(take && take->metadata.frameCount == 2048U && take->metadata.peakLinear > 0.1F,
            error.empty() ? "manual recording failed" : error.c_str());

    AudioMixer mixer(48000);
    mixer.set_capture_sink(recorder, AudioCaptureTap::SynthDry);
    require(recorder->start("synth take"), "synth recorder start failed");
    const std::uint64_t start = mixer.current_frame();
    require(mixer.synthesizer().note_on(60U, 0.8F, 0U, start), "synth note-on failed");
    std::array<float, 512> block{};
    for (int i = 0; i < 32; ++i) mixer.render(block);
    (void)mixer.synthesizer().note_off(60U, 0.0F, 0U, mixer.current_frame());
    for (int i = 0; i < 8; ++i) mixer.render(block);
    auto synthTake = recorder->stop(&error);
    mixer.set_capture_sink({});
    require(synthTake && synthTake->metadata.frameCount >= 10240U && synthTake->metadata.peakLinear > 0.01F,
            error.empty() ? "mixer synth tap recording failed" : error.c_str());
    require(recorder->telemetry().droppedFrames == 0U, "recorder dropped frames in deterministic test");
}

std::string quote(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

void test_ffmpeg_import(const std::filesystem::path& temp) {
    using namespace dve::audio;
    if (!audio_import_capabilities().ffmpeg) return;
    const auto source = make_tone(4800U);
    std::string error;
    const auto wav = temp / "media_source.wav";
    const auto mp3 = temp / "media_source.mp3";
    const auto mp4 = temp / "media_source.mp4";
    require(write_wav_file(wav, source, WavSampleEncoding::Pcm16, &error), error.c_str());
    const std::string mp3Command = "ffmpeg -nostdin -hide_banner -loglevel error -y -i " +
        quote(wav) + " -c:a libmp3lame " + quote(mp3);
    const std::string mp4Command = "ffmpeg -nostdin -hide_banner -loglevel error -y -i " +
        quote(wav) + " -vn -c:a aac " + quote(mp4);
    require(std::system(mp3Command.c_str()) == 0, "failed to create MP3 fixture");
    require(std::system(mp4Command.c_str()) == 0, "failed to create MP4 fixture");
    auto decodedMp3 = import_audio_file(mp3, {}, &error);
    require(decodedMp3 && decodedMp3->metadata.frameCount > 4000U, "MP3 import failed");
    auto decodedMp4 = import_audio_file(mp4, {}, &error);
    require(decodedMp4 && decodedMp4->metadata.frameCount > 4000U, "MP4 audio import failed");
}

} // namespace

int main() {
    const auto temp = std::filesystem::temp_directory_path() / "dve_audio_edit_tests";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp);
    test_edit_session(temp);
    test_recorder_and_mixer_taps();
    test_ffmpeg_import(temp);
    std::filesystem::remove_all(temp);
    std::cout << "audio edit/record/media import tests passed\n";
    return 0;
}
