// libsndfile format coverage and the "no MP3 in the runtime" guarantee (DVE_FETCH_SNDFILE).
//
// Decodes the committed fixtures in tests/data/audio_formats (0.5 s, 440 Hz, amplitude 0.5 mono
// tone encoded by FFmpeg as FLAC, Ogg Vorbis, Ogg Opus and MP3) plus a WAV written here, through
// libsndfile directly and through import_audio_file(). With DVE's own libsndfile build the test
// also requires that MP3 is NOT handled by libsndfile and that neither libmpg123 nor libmp3lame is
// loaded into the process: MP3 is not needed at run time (authoring-time MP3 import goes through
// the optional FFmpeg CLI path).
#include "dve/audio/audio_asset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#if defined(__linux__)
#include <link.h>
#endif

#ifndef DVE_TEST_SNDFILE_PROVIDER
#define DVE_TEST_SNDFILE_PROVIDER "none"
#endif

namespace dve::audio {
#ifdef DVE_HAVE_SNDFILE
// Internal entry point of dve_audio_synth (src/audio/sndfile_decoder.cpp), so a format is known
// to be decoded by libsndfile and not by the FFmpeg fallback.
std::optional<DecodedAudioAsset> import_audio_with_sndfile(const std::filesystem::path& path,
                                                            const AudioImportOptions& options,
                                                            std::string* error);
#endif
}

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint32_t kRate = 48000U;
constexpr std::uint64_t kFrames = 24000U; // 0.5 s
constexpr float kAmplitude = 0.5F;
constexpr float kFrequency = 440.0F;

float reference(std::uint64_t frame) {
    return kAmplitude * std::sin(6.283185307179586F * kFrequency * static_cast<float>(frame) / static_cast<float>(kRate));
}

// Rising zero crossings of channel 0: about kFrequency * duration for the tone.
std::size_t rising_crossings(const dve::audio::DecodedAudioAsset& asset) {
    const std::size_t channels = asset.metadata.channels;
    std::size_t count = 0;
    for (std::size_t frame = 1; frame < asset.metadata.frameCount; ++frame) {
        if (asset.samples[(frame - 1U) * channels] < 0.0F && asset.samples[frame * channels] >= 0.0F) ++count;
    }
    return count;
}

void check_tone(const std::string& label, const std::optional<dve::audio::DecodedAudioAsset>& decoded,
                const std::string& error, bool lossless, bool anyChannels = false) {
    check(decoded.has_value(), label + ": decode failed: " + error);
    if (!decoded) return;
    const auto& meta = decoded->metadata;
    check(meta.sampleRate == kRate, label + ": sample rate " + std::to_string(meta.sampleRate));
    check(anyChannels || meta.channels == 1U, label + ": channels " + std::to_string(meta.channels));
    // Lossy codecs may pad by up to a frame or so; Opus/Vorbis decoders trim pre-skip.
    const auto frames = static_cast<double>(meta.frameCount);
    check(std::abs(frames - static_cast<double>(kFrames)) <= (lossless ? 0.0 : 2048.0),
          label + ": frame count " + std::to_string(meta.frameCount));
    // anyChannels: the FFmpeg path upmixes mono to stereo (-3 dB), so only the pitch is checked.
    if (!anyChannels) {
        check(meta.peakLinear > 0.4F && meta.peakLinear < 0.62F, label + ": peak " + std::to_string(meta.peakLinear));
        const double expectedRms = kAmplitude / std::sqrt(2.0);
        check(std::abs(meta.rmsLinear - expectedRms) < 0.05, label + ": rms " + std::to_string(meta.rmsLinear));
    }
    const std::size_t crossings = rising_crossings(*decoded);
    check(crossings >= 215U && crossings <= 225U, label + ": " + std::to_string(crossings) + " rising zero crossings (440 Hz tone expected ~220)");
    if (lossless) {
        float worst = 0.0F;
        for (std::uint64_t frame = 0; frame < meta.frameCount && frame < kFrames; ++frame)
            worst = std::max(worst, std::abs(decoded->samples[frame] - reference(frame)));
        check(worst < 2.0e-4F, label + ": lossless decode differs from the reference by " + std::to_string(worst));
    }
    std::cout << label << ": " << meta.frameCount << " frames, peak " << meta.peakLinear << ", rms "
              << meta.rmsLinear << ", " << crossings << " crossings\n";
}

#if defined(__linux__)
std::vector<std::string> loaded_objects() {
    std::vector<std::string> names;
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* data) {
        if (info->dlpi_name && *info->dlpi_name)
            static_cast<std::vector<std::string>*>(data)->emplace_back(info->dlpi_name);
        return 0;
    }, &names);
    return names;
}
#endif

} // namespace

int main(int argc, char** argv) {
    using namespace dve::audio;
    if (argc < 2) {
        std::cerr << "usage: dve_audio_sndfile_format_tests <tests/data/audio_formats>\n";
        return 2;
    }
    const std::filesystem::path fixtures = argv[1];
    const std::string provider = DVE_TEST_SNDFILE_PROVIDER;
    const auto caps = audio_import_capabilities();
    std::cout << "libsndfile provider=" << provider << " sndfile=" << caps.sndfile
              << " sndfileMpeg=" << caps.sndfileMpeg << " ffmpeg=" << caps.ffmpeg << '\n';
    check(caps.nativeWav, "native WAV import must always be available");
    check(caps.sndfile == (provider != "none"), "libsndfile capability does not match the build (" + provider + ")");

    const auto temp = std::filesystem::temp_directory_path() / "dve_audio_sndfile_format_tests";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp);

    // WAV: written here, read natively and (when present) through libsndfile.
    DecodedAudioAsset tone;
    tone.metadata.name = "tone";
    tone.metadata.sampleRate = kRate;
    tone.metadata.channels = 1U;
    tone.metadata.frameCount = kFrames;
    tone.metadata.durationSeconds = 0.5;
    tone.samples.resize(kFrames);
    for (std::uint64_t frame = 0; frame < kFrames; ++frame) tone.samples[frame] = reference(frame);
    tone.metadata.contentHash = audio_content_hash(tone.samples, tone.metadata);
    std::string error;
    const auto wav = temp / "tone.wav";
    check(write_wav_file(wav, tone, WavSampleEncoding::Pcm16, &error), "writing the WAV fixture: " + error);
    {
        auto decoded = import_audio_file(wav, {}, &error);
        check_tone("wav (import_audio_file)", decoded, error, true);
    }

#ifdef DVE_HAVE_SNDFILE
    {
        auto decoded = import_audio_with_sndfile(wav, {}, &error);
        check_tone("wav (libsndfile)", decoded, error, true);
    }
    struct Format { const char* file; const char* label; bool lossless; };
    for (const Format format : {Format{"tone.flac", "flac", true}, Format{"tone.ogg", "ogg vorbis", false},
                                Format{"tone.opus", "ogg opus", false}}) {
        const auto path = fixtures / format.file;
        check(std::filesystem::is_regular_file(path), path.string() + " is missing");
        auto direct = import_audio_with_sndfile(path, {}, &error);
        check_tone(std::string(format.label) + " (libsndfile)", direct, error, format.lossless);
        auto general = import_audio_file(path, {}, &error);
        check_tone(std::string(format.label) + " (import_audio_file)", general, error, format.lossless);
    }

    // MP3 is not a runtime format of DVE's libsndfile build.
    const auto mp3 = fixtures / "tone.mp3";
    auto mp3ViaSndfile = import_audio_with_sndfile(mp3, {}, &error);
    if (provider == "fetched") {
        check(!caps.sndfileMpeg, "DVE's libsndfile build reports MPEG support (it must be built with ENABLE_MPEG=OFF)");
        check(!mp3ViaSndfile.has_value(), "DVE's libsndfile build decoded an MP3; it must not need libmpg123");
        std::cout << "mp3 (libsndfile): rejected as expected (" << error << ")\n";
    } else {
        std::cout << "mp3 (libsndfile, " << provider << "): " << (mp3ViaSndfile ? "decoded" : "rejected") << '\n';
    }
    // import_audio_file() still handles MP3 for authoring when the FFmpeg CLI path is built, and
    // otherwise fails cleanly with a reason.
    auto mp3General = import_audio_file(mp3, {}, &error);
    if (caps.ffmpeg) {
        // The FFmpeg CLI path decodes to the engine's stereo layout.
        check_tone("mp3 (import_audio_file via FFmpeg)", mp3General, error, false, true);
    } else if (provider == "fetched") {
        check(!mp3General && error.find("no audio decoder") != std::string::npos,
              "MP3 import without FFmpeg must fail with a reason: " + error);
    }
#else
    std::cout << "libsndfile support is not built; only WAV was checked\n";
#endif

#if defined(__linux__)
    // Nothing MPEG-related may be loaded, whatever was decoded above.
    const auto objects = loaded_objects();
    for (const auto& name : objects) {
        if (name.find("libsndfile") != std::string::npos) std::cout << "loaded: " << name << '\n';
        if (provider == "fetched") {
            check(name.find("libmpg123") == std::string::npos, "libmpg123 is loaded: " + name);
            check(name.find("libmp3lame") == std::string::npos, "libmp3lame is loaded: " + name);
        }
    }
#endif
    std::filesystem::remove_all(temp);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio sndfile format tests passed\n";
    return 0;
}
