#include "dve/audio/audio_time_stretch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace dve::audio {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

bool valid_asset(const DecodedAudioAsset& asset) noexcept {
    return asset.metadata.sampleRate > 0U && (asset.metadata.channels == 1U || asset.metadata.channels == 2U) &&
           asset.metadata.frameCount > 0U &&
           asset.samples.size() >= static_cast<std::size_t>(asset.metadata.frameCount) * asset.metadata.channels;
}

void finish_metadata(DecodedAudioAsset& asset, std::string suffix) {
    asset.metadata.name += std::move(suffix);
    asset.metadata.frameCount = asset.samples.size() / std::max<std::uint8_t>(1U, asset.metadata.channels);
    asset.metadata.durationSeconds = static_cast<double>(asset.metadata.frameCount) /
                                     static_cast<double>(asset.metadata.sampleRate);
    asset.metadata.contentHash = audio_content_hash(asset.samples, asset.metadata);
}

float mono_at(const DecodedAudioAsset& asset, std::uint64_t frame) noexcept {
    const std::size_t index = static_cast<std::size_t>(frame) * asset.metadata.channels;
    if (asset.metadata.channels == 1U) return asset.samples[index];
    return 0.5F * (asset.samples[index] + asset.samples[index + 1U]);
}

DecodedAudioAsset resample_frames(const DecodedAudioAsset& asset, std::uint64_t targetFrames) {
    DecodedAudioAsset result;
    result.metadata = asset.metadata;
    result.metadata.frameCount = targetFrames;
    result.samples.assign(static_cast<std::size_t>(targetFrames) * asset.metadata.channels, 0.0F);
    if (!valid_asset(asset) || targetFrames == 0U) { finish_metadata(result, " [resampled]"); return result; }
    const long double scale = targetFrames > 1U
        ? static_cast<long double>(asset.metadata.frameCount - 1U) / static_cast<long double>(targetFrames - 1U)
        : 0.0L;
    for (std::uint64_t frame = 0U; frame < targetFrames; ++frame) {
        const long double position = static_cast<long double>(frame) * scale;
        const auto left = static_cast<std::uint64_t>(position);
        const auto right = std::min<std::uint64_t>(asset.metadata.frameCount - 1U, left + 1U);
        const float fraction = static_cast<float>(position - static_cast<long double>(left));
        for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel) {
            const float a = asset.samples[static_cast<std::size_t>(left) * asset.metadata.channels + channel];
            const float b = asset.samples[static_cast<std::size_t>(right) * asset.metadata.channels + channel];
            result.samples[static_cast<std::size_t>(frame) * asset.metadata.channels + channel] =
                a + (b - a) * fraction;
        }
    }
    finish_metadata(result, " [resampled]");
    return result;
}

std::uint64_t best_source_start(const DecodedAudioAsset& asset,
                                std::span<const float> output,
                                std::span<const float> weights,
                                std::uint64_t outputStart,
                                std::uint64_t expectedSource,
                                std::uint32_t overlapFrames,
                                std::uint32_t searchRadius) noexcept {
    if (outputStart == 0U || overlapFrames < 8U) return expectedSource;
    const std::uint64_t maximumStart = asset.metadata.frameCount > overlapFrames
        ? asset.metadata.frameCount - overlapFrames : 0U;
    const std::uint64_t begin = expectedSource > searchRadius ? expectedSource - searchRadius : 0U;
    const std::uint64_t end = std::min<std::uint64_t>(maximumStart, expectedSource + searchRadius);
    double bestScore = -std::numeric_limits<double>::infinity();
    std::uint64_t best = std::min(expectedSource, maximumStart);
    const std::uint64_t step = std::max<std::uint64_t>(1U, overlapFrames / 96U);
    for (std::uint64_t candidate = begin; candidate <= end; candidate += step) {
        double dot{}, sourceEnergy{}, outputEnergy{};
        for (std::uint32_t offset = 0U; offset < overlapFrames; offset += 2U) {
            const std::uint64_t outputFrame = outputStart + offset;
            if (outputFrame >= weights.size() || weights[outputFrame] <= 1.0e-9F) continue;
            const float existing = 0.5F * (output[static_cast<std::size_t>(outputFrame) * asset.metadata.channels] +
                output[static_cast<std::size_t>(outputFrame) * asset.metadata.channels + (asset.metadata.channels - 1U)]) /
                weights[outputFrame];
            const float source = mono_at(asset, candidate + offset);
            dot += static_cast<double>(existing) * source;
            sourceEnergy += static_cast<double>(source) * source;
            outputEnergy += static_cast<double>(existing) * existing;
        }
        const double denominator = std::sqrt(sourceEnergy * outputEnergy) + 1.0e-12;
        const double score = dot / denominator - 1.0e-8 * std::abs(static_cast<double>(candidate) - static_cast<double>(expectedSource));
        if (score > bestScore) { bestScore = score; best = candidate; }
        if (end - candidate < step) break;
    }
    return best;
}

DecodedAudioAsset slice_asset(const DecodedAudioAsset& asset, std::uint64_t begin, std::uint64_t count) {
    DecodedAudioAsset result;
    result.metadata = asset.metadata;
    begin = std::min(begin, asset.metadata.frameCount);
    count = std::min(count, asset.metadata.frameCount - begin);
    const auto first = asset.samples.begin() + static_cast<std::ptrdiff_t>(begin * asset.metadata.channels);
    const auto last = first + static_cast<std::ptrdiff_t>(count * asset.metadata.channels);
    result.samples.assign(first, last);
    finish_metadata(result, " [slice]");
    return result;
}

} // namespace

DecodedAudioAsset audio_time_stretch_wsola(const DecodedAudioAsset& asset, double durationRatio) {
    if (!valid_asset(asset) || !std::isfinite(durationRatio)) return {};
    durationRatio = std::clamp(durationRatio, 0.25, 4.0);
    const auto targetFrames = std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::llround(
        static_cast<long double>(asset.metadata.frameCount) * durationRatio)));
    if (targetFrames == asset.metadata.frameCount) return asset;

    DecodedAudioAsset result;
    result.metadata = asset.metadata;
    result.samples.assign(static_cast<std::size_t>(targetFrames) * asset.metadata.channels, 0.0F);
    std::vector<float> weights(static_cast<std::size_t>(targetFrames), 0.0F);
    const std::uint32_t maximumWindow = static_cast<std::uint32_t>(std::min<std::uint64_t>(asset.metadata.frameCount, 4096U));
    const std::uint32_t windowFrames = std::max<std::uint32_t>(32U, std::min(maximumWindow,
        std::max<std::uint32_t>(256U, asset.metadata.sampleRate / 25U)));
    const std::uint32_t outputHop = std::max<std::uint32_t>(16U, windowFrames / 2U);
    const std::uint32_t overlap = windowFrames - outputHop;
    const std::uint32_t searchRadius = std::min<std::uint32_t>(windowFrames / 2U,
        std::max<std::uint32_t>(8U, asset.metadata.sampleRate / 80U));
    const long double inputHop = static_cast<long double>(outputHop) / durationRatio;
    std::vector<float> window(windowFrames);
    for (std::uint32_t frame = 0U; frame < windowFrames; ++frame)
        window[frame] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * frame / std::max(1U, windowFrames - 1U)));

    std::uint64_t grain{};
    for (std::uint64_t outputStart = 0U; outputStart < targetFrames; outputStart += outputHop, ++grain) {
        const std::uint64_t expected = std::min<std::uint64_t>(
            asset.metadata.frameCount > windowFrames ? asset.metadata.frameCount - windowFrames : 0U,
            static_cast<std::uint64_t>(std::llround(static_cast<long double>(grain) * inputHop)));
        const std::uint64_t sourceStart = best_source_start(asset, result.samples, weights, outputStart,
            expected, std::min<std::uint32_t>(overlap, static_cast<std::uint32_t>(targetFrames - outputStart)), searchRadius);
        const std::uint64_t available = std::min<std::uint64_t>(windowFrames, targetFrames - outputStart);
        for (std::uint64_t offset = 0U; offset < available; ++offset) {
            const std::uint64_t sourceFrame = std::min<std::uint64_t>(asset.metadata.frameCount - 1U, sourceStart + offset);
            const float weight = window[static_cast<std::size_t>(offset)];
            weights[static_cast<std::size_t>(outputStart + offset)] += weight;
            for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel)
                result.samples[static_cast<std::size_t>(outputStart + offset) * asset.metadata.channels + channel] +=
                    asset.samples[static_cast<std::size_t>(sourceFrame) * asset.metadata.channels + channel] * weight;
        }
    }
    for (std::uint64_t frame = 0U; frame < targetFrames; ++frame) {
        const float weight = weights[static_cast<std::size_t>(frame)];
        if (weight <= 1.0e-8F) continue;
        for (std::uint8_t channel = 0U; channel < asset.metadata.channels; ++channel)
            result.samples[static_cast<std::size_t>(frame) * asset.metadata.channels + channel] /= weight;
    }
    finish_metadata(result, " [WSOLA " + std::to_string(durationRatio) + "x]");
    return result;
}

DecodedAudioAsset audio_pitch_shift_wsola(const DecodedAudioAsset& asset, double semitones) {
    if (!valid_asset(asset) || !std::isfinite(semitones)) return {};
    semitones = std::clamp(semitones, -24.0, 24.0);
    const double factor = std::pow(2.0, semitones / 12.0);
    const auto resampledFrames = std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::llround(
        static_cast<long double>(asset.metadata.frameCount) / factor)));
    auto shifted = resample_frames(asset, resampledFrames);
    shifted = audio_time_stretch_wsola(shifted,
        static_cast<double>(asset.metadata.frameCount) / static_cast<double>(resampledFrames));
    if (shifted.metadata.frameCount != asset.metadata.frameCount) shifted = resample_frames(shifted, asset.metadata.frameCount);
    shifted.metadata.name = asset.metadata.name + " [pitch " + std::to_string(semitones) + " st]";
    shifted.metadata.contentHash = audio_content_hash(shifted.samples, shifted.metadata);
    return shifted;
}

DecodedAudioAsset audio_render_warp_markers(const DecodedAudioAsset& asset,
                                             std::span<const AudioWarpMarker> markers) {
    if (!valid_asset(asset)) return {};
    std::vector<AudioWarpMarker> resolved(markers.begin(), markers.end());
    std::sort(resolved.begin(), resolved.end(), [](const auto& a, const auto& b) { return a.sourceFrame < b.sourceFrame; });
    if (resolved.empty() || resolved.front().sourceFrame != 0U || resolved.front().timelineFrame != 0U)
        resolved.insert(resolved.begin(), {});
    for (std::size_t i = 1U; i < resolved.size(); ++i) {
        if (resolved[i].sourceFrame <= resolved[i - 1U].sourceFrame ||
            resolved[i].timelineFrame <= resolved[i - 1U].timelineFrame ||
            resolved[i].sourceFrame > asset.metadata.frameCount) return {};
    }
    if (resolved.back().sourceFrame < asset.metadata.frameCount) {
        const auto& last = resolved.back();
        const std::uint64_t sourceTail = asset.metadata.frameCount - last.sourceFrame;
        resolved.push_back({asset.metadata.frameCount, last.timelineFrame + sourceTail});
    }
    DecodedAudioAsset result;
    result.metadata = asset.metadata;
    result.samples.reserve(static_cast<std::size_t>(resolved.back().timelineFrame) * asset.metadata.channels);
    for (std::size_t index = 1U; index < resolved.size(); ++index) {
        const auto sourceCount = resolved[index].sourceFrame - resolved[index - 1U].sourceFrame;
        const auto timelineCount = resolved[index].timelineFrame - resolved[index - 1U].timelineFrame;
        auto segment = slice_asset(asset, resolved[index - 1U].sourceFrame, sourceCount);
        auto stretched = audio_time_stretch_wsola(segment,
            static_cast<double>(timelineCount) / static_cast<double>(sourceCount));
        if (stretched.metadata.frameCount != timelineCount) stretched = resample_frames(stretched, timelineCount);
        result.samples.insert(result.samples.end(), stretched.samples.begin(), stretched.samples.end());
    }
    finish_metadata(result, " [warped]");
    return result;
}

} // namespace dve::audio
