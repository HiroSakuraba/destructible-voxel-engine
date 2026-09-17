#include "dve/audio/audio_asset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

extern "C" {
using sf_count_t = std::int64_t;
struct SNDFILE_tag;
using SNDFILE = SNDFILE_tag;
struct SF_INFO {
    sf_count_t frames;
    int samplerate;
    int channels;
    int format;
    int sections;
    int seekable;
};
SNDFILE* sf_open(const char* path, int mode, SF_INFO* info);
sf_count_t sf_readf_float(SNDFILE* file, float* data, sf_count_t frames);
int sf_close(SNDFILE* file);
const char* sf_strerror(SNDFILE* file);
}

namespace dve::audio {
namespace {
constexpr int kSfmRead = 0x10;

std::vector<float> resample(std::span<const float> input, std::uint8_t channels,
                            std::uint32_t sourceRate, std::uint32_t targetRate) {
    if (sourceRate == targetRate) return {input.begin(), input.end()};
    const std::size_t sourceFrames = input.size() / channels;
    const std::size_t targetFrames = static_cast<std::size_t>(std::llround(
        static_cast<long double>(sourceFrames) * targetRate / sourceRate));
    std::vector<float> output(targetFrames * channels);
    const double scale = static_cast<double>(sourceRate) / targetRate;
    for (std::size_t frame = 0; frame < targetFrames; ++frame) {
        const double position = std::min(static_cast<double>(sourceFrames - 1U), static_cast<double>(frame) * scale);
        const auto i0 = static_cast<std::size_t>(position);
        const auto i1 = std::min(i0 + 1U, sourceFrames - 1U);
        const float t = static_cast<float>(position - static_cast<double>(i0));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const float a = input[i0 * channels + channel];
            const float b = input[i1 * channels + channel];
            output[frame * channels + channel] = a + (b - a) * t;
        }
    }
    return output;
}
}

std::optional<DecodedAudioAsset> import_audio_with_sndfile(const std::filesystem::path& path,
                                                            const AudioImportOptions& options,
                                                            std::string* error) {
    SF_INFO info{};
    SNDFILE* file = sf_open(path.string().c_str(), kSfmRead, &info);
    if (!file) { if (error) *error = "libsndfile: unable to open source audio"; return std::nullopt; }
    auto close = [&] { (void)sf_close(file); file = nullptr; };
    if (info.frames <= 0 || info.frames > static_cast<sf_count_t>(0x7fffffff) ||
        info.samplerate < 8000 || info.samplerate > 384000 || info.channels < 1 || info.channels > 2) {
        if (error) *error = "libsndfile: unsupported rate, channel count, or duration";
        close();
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(info.frames) * static_cast<std::size_t>(info.channels));
    const sf_count_t framesRead = sf_readf_float(file, samples.data(), info.frames);
    if (framesRead != info.frames) {
        if (error) *error = std::string("libsndfile: decode failed: ") + sf_strerror(file);
        close();
        return std::nullopt;
    }
    close();
    for (float& sample : samples) if (!std::isfinite(sample)) sample = 0.0F;
    const auto channels = static_cast<std::uint8_t>(info.channels);
    const auto targetRate = std::clamp(options.targetSampleRate, 8000U, 384000U);
    if (static_cast<std::uint32_t>(info.samplerate) != targetRate)
        samples = resample(samples, channels, static_cast<std::uint32_t>(info.samplerate), targetRate);
    if (options.normalize) {
        float peak{};
        for (const float sample : samples) peak = std::max(peak, std::abs(sample));
        if (peak > 1.0e-8F) {
            const float gain = std::clamp(options.normalizePeak, 0.01F, 1.0F) / peak;
            for (float& sample : samples) sample *= gain;
        }
    }
    DecodedAudioAsset result;
    result.metadata.name = path.stem().string();
    result.metadata.sampleRate = targetRate;
    result.metadata.channels = channels;
    result.metadata.storagePolicy = options.storagePolicy;
    result.metadata.frameCount = samples.size() / channels;
    result.metadata.durationSeconds = static_cast<double>(result.metadata.frameCount) / targetRate;
    double energy{};
    for (const float sample : samples) {
        result.metadata.peakLinear = std::max(result.metadata.peakLinear, std::abs(sample));
        energy += static_cast<double>(sample) * sample;
    }
    result.metadata.rmsLinear = static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
    result.metadata.approximateLoudnessDbfs = result.metadata.rmsLinear > 1.0e-9F
        ? 20.0F * std::log10(result.metadata.rmsLinear) : -120.0F;
    result.samples = std::move(samples);
    result.metadata.contentHash = audio_content_hash(result.samples, result.metadata);
    return result;
}

} // namespace dve::audio
