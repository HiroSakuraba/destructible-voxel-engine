#include "dve/audio/audio_repair.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "dve/audio/audio_metering.hpp"

namespace dve::audio {
namespace {

DecodedAudioAsset copy_asset(const DecodedAudioAsset& asset, std::string suffix) {
    DecodedAudioAsset result = asset;
    if (!suffix.empty()) result.metadata.name += std::move(suffix);
    return result;
}

void refresh_metadata(DecodedAudioAsset& asset) {
    asset.metadata.frameCount = asset.metadata.channels == 0U ? 0U :
        static_cast<std::uint64_t>(asset.samples.size() / asset.metadata.channels);
    asset.metadata.durationSeconds = asset.metadata.sampleRate == 0U ? 0.0 :
        static_cast<double>(asset.metadata.frameCount) / static_cast<double>(asset.metadata.sampleRate);
    double energy{};
    asset.metadata.peakLinear = 0.0F;
    for (const float value : asset.samples) {
        asset.metadata.peakLinear = std::max(asset.metadata.peakLinear, std::abs(value));
        energy += static_cast<double>(value) * value;
    }
    asset.metadata.rmsLinear = asset.samples.empty() ? 0.0F :
        static_cast<float>(std::sqrt(energy / static_cast<double>(asset.samples.size())));
    asset.metadata.approximateLoudnessDbfs = asset.metadata.rmsLinear > 1.0e-12F
        ? 20.0F * std::log10(asset.metadata.rmsLinear) : -120.0F;
    asset.metadata.contentHash = audio_content_hash(asset.samples, asset.metadata);
}

} // namespace

DecodedAudioAsset audio_remove_dc(const DecodedAudioAsset& asset) {
    auto result = copy_asset(asset, " [DC removed]");
    if (asset.metadata.channels == 0U || asset.samples.empty()) return result;
    std::vector<long double> means(asset.metadata.channels, 0.0L);
    for (std::uint64_t frame = 0U; frame < asset.metadata.frameCount; ++frame)
        for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel)
            means[channel] += asset.samples[static_cast<std::size_t>(frame) * asset.metadata.channels + channel];
    for (auto& mean : means) mean /= static_cast<long double>(asset.metadata.frameCount);
    for (std::uint64_t frame = 0U; frame < asset.metadata.frameCount; ++frame)
        for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel)
            result.samples[static_cast<std::size_t>(frame) * asset.metadata.channels + channel] -=
                static_cast<float>(means[channel]);
    refresh_metadata(result);
    return result;
}

DecodedAudioAsset audio_normalize_peak(const DecodedAudioAsset& asset, float targetPeak) {
    auto result = copy_asset(asset, " [peak normalized]");
    targetPeak = std::clamp(targetPeak, 0.0F, 1.0F);
    float peak{};
    for (const float value : result.samples) peak = std::max(peak, std::abs(value));
    if (peak > 1.0e-12F) {
        const float gain = targetPeak / peak;
        for (float& value : result.samples) value *= gain;
    }
    refresh_metadata(result);
    return result;
}

DecodedAudioAsset audio_normalize_loudness(const DecodedAudioAsset& asset,
                                            float targetLufs,
                                            float maximumTruePeak) {
    auto result = copy_asset(asset, " [loudness normalized]");
    targetLufs = std::clamp(targetLufs, -70.0F, 0.0F);
    maximumTruePeak = std::clamp(maximumTruePeak, 0.01F, 1.0F);
    const auto report = measure_audio_loudness_ebu_r128(asset, false);
    if (report.integratedLufs > -119.0F) {
        float gain = std::pow(10.0F, (targetLufs - report.integratedLufs) / 20.0F);
        if (report.truePeak > 1.0e-12F) gain = std::min(gain, maximumTruePeak / report.truePeak);
        for (float& value : result.samples) value *= gain;
    }
    refresh_metadata(result);
    return result;
}

DecodedAudioAsset audio_trim_silence(const DecodedAudioAsset& asset,
                                      const AudioSilenceTrimOptions& options) {
    if (asset.metadata.channels == 0U || asset.metadata.frameCount == 0U) return asset;
    const float threshold = std::clamp(options.thresholdLinear, 0.0F, 1.0F);
    auto frame_peak = [&](std::uint64_t frame) {
        float peak{};
        for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel)
            peak = std::max(peak, std::abs(asset.samples[static_cast<std::size_t>(frame) * asset.metadata.channels + channel]));
        return peak;
    };
    std::uint64_t first{};
    while (first < asset.metadata.frameCount && frame_peak(first) <= threshold) ++first;
    std::uint64_t last = asset.metadata.frameCount;
    while (last > first && frame_peak(last - 1U) <= threshold) --last;
    if (first < options.minimumSilenceFrames) first = 0U;
    else first -= std::min<std::uint64_t>(first, options.preserveFrames);
    const std::uint64_t trailing = asset.metadata.frameCount - last;
    if (trailing < options.minimumSilenceFrames) last = asset.metadata.frameCount;
    else last = std::min<std::uint64_t>(asset.metadata.frameCount, last + options.preserveFrames);
    auto result = copy_asset(asset, " [silence trimmed]");
    result.samples.assign(asset.samples.begin() + static_cast<std::ptrdiff_t>(first * asset.metadata.channels),
                          asset.samples.begin() + static_cast<std::ptrdiff_t>(last * asset.metadata.channels));
    refresh_metadata(result);
    return result;
}

DecodedAudioAsset audio_apply_fades(const DecodedAudioAsset& asset,
                                     std::uint64_t fadeInFrames,
                                     std::uint64_t fadeOutFrames,
                                     bool equalPower) {
    auto result = copy_asset(asset, " [faded]");
    fadeInFrames = std::min(fadeInFrames, asset.metadata.frameCount);
    fadeOutFrames = std::min(fadeOutFrames, asset.metadata.frameCount);
    auto curve = [equalPower](float t) {
        t = std::clamp(t, 0.0F, 1.0F);
        return equalPower ? std::sin(t * 1.5707963267948966F) : t;
    };
    for (std::uint64_t frame = 0U; frame < asset.metadata.frameCount; ++frame) {
        float gain = 1.0F;
        if (fadeInFrames > 0U && frame < fadeInFrames)
            gain *= curve(static_cast<float>(frame) / static_cast<float>(fadeInFrames));
        if (fadeOutFrames > 0U) {
            const std::uint64_t remaining = asset.metadata.frameCount - frame;
            if (remaining < fadeOutFrames)
                gain *= curve(static_cast<float>(remaining) / static_cast<float>(fadeOutFrames));
        }
        for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel)
            result.samples[static_cast<std::size_t>(frame) * asset.metadata.channels + channel] *= gain;
    }
    refresh_metadata(result);
    return result;
}

DecodedAudioAsset audio_reverse(const DecodedAudioAsset& asset) {
    auto result = copy_asset(asset, " [reversed]");
    const std::size_t channels = asset.metadata.channels;
    if (channels == 0U) return result;
    for (std::uint64_t left = 0U, right = asset.metadata.frameCount == 0U ? 0U : asset.metadata.frameCount - 1U;
         left < right; ++left, --right) {
        for (std::size_t channel = 0U; channel < channels; ++channel)
            std::swap(result.samples[static_cast<std::size_t>(left) * channels + channel],
                      result.samples[static_cast<std::size_t>(right) * channels + channel]);
    }
    refresh_metadata(result);
    return result;
}

std::optional<DecodedAudioAsset> audio_convert_channels(const DecodedAudioAsset& asset,
                                                         std::uint8_t channels) {
    if ((channels != 1U && channels != 2U) || asset.metadata.channels == 0U ||
        asset.metadata.channels > 2U) return std::nullopt;
    if (channels == asset.metadata.channels) return asset;
    auto result = copy_asset(asset, channels == 1U ? " [mono]" : " [stereo]");
    result.metadata.channels = channels;
    result.samples.resize(static_cast<std::size_t>(asset.metadata.frameCount) * channels);
    for (std::uint64_t frame = 0U; frame < asset.metadata.frameCount; ++frame) {
        if (channels == 1U) {
            const std::size_t index = static_cast<std::size_t>(frame) * 2U;
            result.samples[static_cast<std::size_t>(frame)] = 0.5F * (asset.samples[index] + asset.samples[index + 1U]);
        } else {
            const float value = asset.samples[static_cast<std::size_t>(frame)];
            result.samples[static_cast<std::size_t>(frame) * 2U] = value;
            result.samples[static_cast<std::size_t>(frame) * 2U + 1U] = value;
        }
    }
    refresh_metadata(result);
    return result;
}

} // namespace dve::audio
