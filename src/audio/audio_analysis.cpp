#include "dve/audio/audio_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::audio {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

float mono_sample(const DecodedAudioAsset& asset, std::uint64_t frame) noexcept {
    if (frame >= asset.metadata.frameCount || asset.metadata.channels == 0U) return 0.0F;
    const std::size_t index = static_cast<std::size_t>(frame) * asset.metadata.channels;
    if (asset.metadata.channels == 1U) return asset.samples[index];
    return 0.5F * (asset.samples[index] + asset.samples[index + 1U]);
}

AudioWaveformPeak combine_peaks(const AudioWaveformPeak& a, const AudioWaveformPeak& b) noexcept {
    return {
        std::min(a.minimumLeft, b.minimumLeft), std::max(a.maximumLeft, b.maximumLeft),
        std::min(a.minimumRight, b.minimumRight), std::max(a.maximumRight, b.maximumRight)
    };
}

} // namespace

AudioWaveformPeakCache build_audio_waveform_peak_cache(const DecodedAudioAsset& asset,
                                                        std::uint32_t baseFramesPerPeak,
                                                        std::size_t maximumLevels) {
    AudioWaveformPeakCache cache;
    cache.contentHash = asset.metadata.contentHash;
    cache.sampleRate = asset.metadata.sampleRate;
    cache.channels = asset.metadata.channels;
    cache.frameCount = asset.metadata.frameCount;
    if (asset.metadata.channels == 0U || asset.metadata.channels > 2U || asset.metadata.frameCount == 0U ||
        asset.samples.empty() || maximumLevels == 0U) return cache;
    baseFramesPerPeak = std::max<std::uint32_t>(1U, baseFramesPerPeak);
    AudioWaveformPeakLevel base;
    base.framesPerPeak = baseFramesPerPeak;
    const std::uint64_t peakCount64 = (asset.metadata.frameCount + baseFramesPerPeak - 1U) / baseFramesPerPeak;
    if (peakCount64 > std::numeric_limits<std::size_t>::max()) return cache;
    base.peaks.reserve(static_cast<std::size_t>(peakCount64));
    for (std::uint64_t begin = 0U; begin < asset.metadata.frameCount; begin += baseFramesPerPeak) {
        const std::uint64_t end = std::min<std::uint64_t>(asset.metadata.frameCount, begin + baseFramesPerPeak);
        AudioWaveformPeak peak{1.0F, -1.0F, 1.0F, -1.0F};
        for (std::uint64_t frame = begin; frame < end; ++frame) {
            const std::size_t index = static_cast<std::size_t>(frame) * asset.metadata.channels;
            const float left = asset.samples[index];
            const float right = asset.metadata.channels == 2U ? asset.samples[index + 1U] : left;
            peak.minimumLeft = std::min(peak.minimumLeft, left);
            peak.maximumLeft = std::max(peak.maximumLeft, left);
            peak.minimumRight = std::min(peak.minimumRight, right);
            peak.maximumRight = std::max(peak.maximumRight, right);
        }
        base.peaks.push_back(peak);
    }
    cache.levels.push_back(std::move(base));
    while (cache.levels.size() < maximumLevels && cache.levels.back().peaks.size() > 1U) {
        const auto& previous = cache.levels.back();
        AudioWaveformPeakLevel next;
        if (previous.framesPerPeak > std::numeric_limits<std::uint32_t>::max() / 2U) break;
        next.framesPerPeak = previous.framesPerPeak * 2U;
        next.peaks.reserve((previous.peaks.size() + 1U) / 2U);
        for (std::size_t i = 0; i < previous.peaks.size(); i += 2U) {
            next.peaks.push_back(i + 1U < previous.peaks.size()
                ? combine_peaks(previous.peaks[i], previous.peaks[i + 1U]) : previous.peaks[i]);
        }
        cache.levels.push_back(std::move(next));
    }
    return cache;
}

const AudioWaveformPeakLevel* choose_audio_waveform_peak_level(const AudioWaveformPeakCache& cache,
                                                                double framesPerPixel) noexcept {
    if (cache.levels.empty()) return nullptr;
    framesPerPixel = std::max(1.0, framesPerPixel);
    const AudioWaveformPeakLevel* chosen = &cache.levels.front();
    for (const auto& level : cache.levels) {
        if (static_cast<double>(level.framesPerPeak) <= framesPerPixel * 2.0) chosen = &level;
        else break;
    }
    return chosen;
}

AudioSpectrogram build_audio_spectrogram(const DecodedAudioAsset& asset,
                                          std::uint32_t windowFrames,
                                          std::uint32_t hopFrames,
                                          std::uint32_t maximumFrequencyBins,
                                          std::uint32_t maximumTimeBins,
                                          float floorDecibels) {
    AudioSpectrogram result;
    result.sampleRate = asset.metadata.sampleRate;
    result.floorDecibels = std::min(-1.0F, floorDecibels);
    if (asset.metadata.frameCount == 0U || asset.metadata.channels == 0U || asset.samples.empty()) return result;
    windowFrames = std::clamp<std::uint32_t>(windowFrames, 32U, 4096U);
    hopFrames = std::clamp<std::uint32_t>(hopFrames, 1U, windowFrames);
    maximumFrequencyBins = std::clamp<std::uint32_t>(maximumFrequencyBins, 1U, windowFrames / 2U + 1U);
    maximumTimeBins = std::max<std::uint32_t>(1U, maximumTimeBins);
    const std::uint64_t naturalTimeBins = asset.metadata.frameCount <= windowFrames ? 1U :
        1U + (asset.metadata.frameCount - windowFrames) / hopFrames;
    const std::uint64_t stride = std::max<std::uint64_t>(1U, (naturalTimeBins + maximumTimeBins - 1U) / maximumTimeBins);
    result.windowFrames = windowFrames;
    result.hopFrames = hopFrames * static_cast<std::uint32_t>(std::min<std::uint64_t>(stride, std::numeric_limits<std::uint32_t>::max() / hopFrames));
    result.frequencyBins = maximumFrequencyBins;
    result.timeBins = static_cast<std::uint32_t>((naturalTimeBins + stride - 1U) / stride);
    result.decibels.resize(static_cast<std::size_t>(result.timeBins) * result.frequencyBins, result.floorDecibels);
    for (std::uint32_t time = 0U; time < result.timeBins; ++time) {
        const std::uint64_t naturalIndex = static_cast<std::uint64_t>(time) * stride;
        const std::uint64_t start = naturalIndex * hopFrames;
        for (std::uint32_t bin = 0U; bin < result.frequencyBins; ++bin) {
            double real{};
            double imaginary{};
            for (std::uint32_t n = 0U; n < windowFrames; ++n) {
                const std::uint64_t frame = std::min<std::uint64_t>(asset.metadata.frameCount - 1U, start + n);
                const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                                         static_cast<double>(windowFrames - 1U));
                const double angle = -2.0 * kPi * static_cast<double>(bin) * static_cast<double>(n) /
                                     static_cast<double>(windowFrames);
                const double sample = static_cast<double>(mono_sample(asset, frame)) * window;
                real += sample * std::cos(angle);
                imaginary += sample * std::sin(angle);
            }
            const double magnitude = std::sqrt(real * real + imaginary * imaginary) /
                                     static_cast<double>(windowFrames);
            const float decibels = magnitude > 1.0e-12 ? static_cast<float>(20.0 * std::log10(magnitude)) : result.floorDecibels;
            result.decibels[static_cast<std::size_t>(time) * result.frequencyBins + bin] =
                std::max(result.floorDecibels, decibels);
        }
    }
    return result;
}

AudioAnalysisReport analyze_audio_asset(const DecodedAudioAsset& asset,
                                        float silenceThreshold,
                                        float transientSensitivity,
                                        double minimumTempoBpm,
                                        double maximumTempoBpm) {
    AudioAnalysisReport report;
    if (asset.metadata.frameCount == 0U || asset.metadata.channels == 0U || asset.samples.empty()) return report;
    silenceThreshold = std::clamp(silenceThreshold, 0.0F, 0.25F);
    transientSensitivity = std::clamp(transientSensitivity, 0.1F, 10.0F);
    minimumTempoBpm = std::clamp(minimumTempoBpm, 30.0, 300.0);
    maximumTempoBpm = std::clamp(maximumTempoBpm, minimumTempoBpm, 400.0);
    double sum{};
    double energy{};
    float previousLeft{};
    float previousRight{};
    for (std::uint64_t frame = 0U; frame < asset.metadata.frameCount; ++frame) {
        const std::size_t index = static_cast<std::size_t>(frame) * asset.metadata.channels;
        const float left = asset.samples[index];
        const float right = asset.metadata.channels == 2U ? asset.samples[index + 1U] : left;
        report.samplePeak = std::max({report.samplePeak, std::abs(left), std::abs(right)});
        report.approximateTruePeak = std::max(report.approximateTruePeak,
            std::max(std::abs(left), std::abs(right)));
        for (int step = 1; step < 4; ++step) {
            const float t = static_cast<float>(step) * 0.25F;
            report.approximateTruePeak = std::max(report.approximateTruePeak,
                std::max(std::abs(previousLeft + (left - previousLeft) * t),
                         std::abs(previousRight + (right - previousRight) * t)));
        }
        previousLeft = left;
        previousRight = right;
        if (std::abs(left) >= 1.0F) ++report.clippedSampleCount;
        if (std::abs(right) >= 1.0F) ++report.clippedSampleCount;
        const float mono = 0.5F * (left + right);
        sum += mono;
        energy += static_cast<double>(left) * left + static_cast<double>(right) * right;
        if (std::max(std::abs(left), std::abs(right)) <= silenceThreshold) ++report.silentFrameCount;
    }
    const double sampleCount = static_cast<double>(asset.metadata.frameCount) * 2.0;
    report.rms = static_cast<float>(std::sqrt(energy / std::max(1.0, sampleCount)));
    report.dcOffset = static_cast<float>(sum / static_cast<double>(asset.metadata.frameCount));
    report.crestFactor = report.rms > 1.0e-12F ? report.samplePeak / report.rms : 0.0F;
    report.approximateIntegratedLufs = report.rms > 1.0e-9F
        ? static_cast<float>(-0.691 + 20.0 * std::log10(report.rms)) : -120.0F;

    const std::uint32_t windowFrames = std::max<std::uint32_t>(64U, asset.metadata.sampleRate / 200U);
    const std::uint32_t hopFrames = std::max<std::uint32_t>(32U, windowFrames / 2U);
    std::vector<float> envelope;
    for (std::uint64_t start = 0U; start < asset.metadata.frameCount; start += hopFrames) {
        const std::uint64_t end = std::min<std::uint64_t>(asset.metadata.frameCount, start + windowFrames);
        double windowEnergy{};
        for (std::uint64_t frame = start; frame < end; ++frame) {
            const float sample = mono_sample(asset, frame);
            windowEnergy += static_cast<double>(sample) * sample;
        }
        envelope.push_back(static_cast<float>(std::sqrt(windowEnergy / static_cast<double>(std::max<std::uint64_t>(1U, end - start)))));
    }
    std::vector<float> flux(envelope.size(), 0.0F);
    float meanFlux{};
    for (std::size_t i = 1; i < envelope.size(); ++i) {
        flux[i] = std::max(0.0F, envelope[i] - envelope[i - 1U]);
        meanFlux += flux[i];
    }
    if (flux.size() > 1U) meanFlux /= static_cast<float>(flux.size() - 1U);
    const float threshold = meanFlux * transientSensitivity;
    for (std::size_t i = 1U; i + 1U < flux.size(); ++i) {
        if (flux[i] > threshold && flux[i] >= flux[i - 1U] && flux[i] > flux[i + 1U])
            report.transientFrames.push_back(static_cast<std::uint64_t>(i) * hopFrames);
    }

    if (envelope.size() > 4U && asset.metadata.sampleRate > 0U) {
        const double envelopeRate = static_cast<double>(asset.metadata.sampleRate) / hopFrames;
        const std::size_t minLag = static_cast<std::size_t>(std::max(1.0, std::floor(envelopeRate * 60.0 / maximumTempoBpm)));
        const std::size_t maxLag = static_cast<std::size_t>(std::ceil(envelopeRate * 60.0 / minimumTempoBpm));
        double best{};
        std::size_t bestLag{};
        double zeroLag{};
        for (float value : flux) zeroLag += static_cast<double>(value) * value;
        for (std::size_t lag = minLag; lag <= maxLag && lag < flux.size(); ++lag) {
            double correlation{};
            for (std::size_t i = lag; i < flux.size(); ++i)
                correlation += static_cast<double>(flux[i]) * flux[i - lag];
            if (correlation > best) { best = correlation; bestLag = lag; }
        }
        if (bestLag > 0U) {
            report.estimatedTempoBpm = envelopeRate * 60.0 / static_cast<double>(bestLag);
            report.tempoConfidence = zeroLag > 1.0e-12 ? static_cast<float>(std::clamp(best / zeroLag, 0.0, 1.0)) : 0.0F;
        }
    }
    return report;
}

} // namespace dve::audio
