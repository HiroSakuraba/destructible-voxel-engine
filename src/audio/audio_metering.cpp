#include "dve/audio/audio_metering.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace dve::audio {
namespace {

constexpr double kLoudnessOffset = -0.691;
constexpr double kPi = 3.1415926535897932384626433832795;

struct Biquad {
    double b0{}, b1{}, b2{}, a1{}, a2{};
    double z1{}, z2{};
    double process(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad pre_filter() noexcept {
    return {1.53512485958697, -2.69169618940638, 1.19839281085285,
            -1.69065929318241, 0.73248077421585, 0.0, 0.0};
}

Biquad rlb_filter() noexcept {
    return {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621, 0.0, 0.0};
}

std::vector<float> resample_to_48k(const DecodedAudioAsset& asset) {
    const std::uint8_t channels = asset.metadata.channels;
    if (asset.metadata.sampleRate == 48000U) return asset.samples;
    if (channels == 0U || asset.metadata.sampleRate == 0U || asset.metadata.frameCount == 0U) return {};
    const long double ratio = 48000.0L / static_cast<long double>(asset.metadata.sampleRate);
    const long double framesValue = static_cast<long double>(asset.metadata.frameCount) * ratio;
    if (framesValue > static_cast<long double>(std::numeric_limits<std::size_t>::max() / channels)) return {};
    const std::size_t outputFrames = static_cast<std::size_t>(std::llround(framesValue));
    std::vector<float> result(outputFrames * channels);
    for (std::size_t frame = 0; frame < outputFrames; ++frame) {
        const long double sourcePosition = static_cast<long double>(frame) / ratio;
        const std::uint64_t leftFrame = static_cast<std::uint64_t>(sourcePosition);
        const std::uint64_t rightFrame = std::min<std::uint64_t>(asset.metadata.frameCount - 1U, leftFrame + 1U);
        const float t = static_cast<float>(sourcePosition - static_cast<long double>(leftFrame));
        for (std::uint8_t channel = 0U; channel < channels; ++channel) {
            const float a = asset.samples[static_cast<std::size_t>(leftFrame) * channels + channel];
            const float b = asset.samples[static_cast<std::size_t>(rightFrame) * channels + channel];
            result[frame * channels + channel] = a + (b - a) * t;
        }
    }
    return result;
}

float loudness_from_energy(double energy) noexcept {
    return energy > 1.0e-20 ? static_cast<float>(kLoudnessOffset + 10.0 * std::log10(energy)) : -120.0F;
}

std::vector<double> block_energies(std::span<const double> perFrameEnergy,
                                   std::size_t windowFrames,
                                   std::size_t hopFrames) {
    std::vector<double> result;
    if (perFrameEnergy.empty() || windowFrames == 0U || hopFrames == 0U) return result;
    std::vector<long double> prefix(perFrameEnergy.size() + 1U, 0.0L);
    for (std::size_t i = 0; i < perFrameEnergy.size(); ++i)
        prefix[i + 1U] = prefix[i] + static_cast<long double>(perFrameEnergy[i]);
    if (perFrameEnergy.size() <= windowFrames) {
        result.push_back(static_cast<double>(prefix.back() / static_cast<long double>(perFrameEnergy.size())));
        return result;
    }
    for (std::size_t start = 0U; start + windowFrames <= perFrameEnergy.size(); start += hopFrames) {
        const long double sum = prefix[start + windowFrames] - prefix[start];
        result.push_back(static_cast<double>(sum / static_cast<long double>(windowFrames)));
    }
    return result;
}

float integrated_loudness(const std::vector<double>& energies, std::uint64_t* gatedCount = nullptr) noexcept {
    std::vector<double> absoluteGated;
    absoluteGated.reserve(energies.size());
    for (const double energy : energies) {
        if (loudness_from_energy(energy) >= -70.0F) absoluteGated.push_back(energy);
    }
    if (absoluteGated.empty()) { if (gatedCount) *gatedCount = 0U; return -120.0F; }
    long double absoluteMean{};
    for (const double energy : absoluteGated) absoluteMean += energy;
    absoluteMean /= static_cast<long double>(absoluteGated.size());
    const float relativeThreshold = loudness_from_energy(static_cast<double>(absoluteMean)) - 10.0F;
    long double relativeMean{};
    std::uint64_t count{};
    for (const double energy : absoluteGated) {
        if (loudness_from_energy(energy) >= relativeThreshold) {
            relativeMean += energy;
            ++count;
        }
    }
    if (gatedCount) *gatedCount = count;
    return count == 0U ? -120.0F :
        loudness_from_energy(static_cast<double>(relativeMean / static_cast<long double>(count)));
}

float percentile(std::vector<float> values, double fraction) {
    if (values.empty()) return -120.0F;
    fraction = std::clamp(fraction, 0.0, 1.0);
    const std::size_t index = static_cast<std::size_t>(std::llround(
        fraction * static_cast<double>(values.size() - 1U)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

double sinc(double x) noexcept {
    if (std::abs(x) < 1.0e-12) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

float estimate_true_peak(std::span<const float> samples, std::uint8_t channels) noexcept {
    if (samples.empty() || channels == 0U) return 0.0F;
    const std::size_t frames = samples.size() / channels;
    float peak{};
    constexpr int radius = 8;
    for (std::uint8_t channel = 0U; channel < channels; ++channel) {
        for (std::size_t frame = 0U; frame < frames; ++frame) {
            peak = std::max(peak, std::abs(samples[frame * channels + channel]));
            for (int phase = 1; phase < 4; ++phase) {
                const double position = static_cast<double>(frame) + static_cast<double>(phase) * 0.25;
                double value{};
                double weightSum{};
                const int center = static_cast<int>(std::floor(position));
                for (int tap = center - radius + 1; tap <= center + radius; ++tap) {
                    const int clamped = std::clamp(tap, 0, static_cast<int>(frames) - 1);
                    const double distance = position - static_cast<double>(tap);
                    const double normalized = distance / static_cast<double>(radius);
                    const double window = std::abs(normalized) <= 1.0
                        ? 0.5 + 0.5 * std::cos(kPi * normalized) : 0.0;
                    const double weight = sinc(distance) * window;
                    value += static_cast<double>(samples[static_cast<std::size_t>(clamped) * channels + channel]) * weight;
                    weightSum += weight;
                }
                if (std::abs(weightSum) > 1.0e-12) value /= weightSum;
                peak = std::max(peak, static_cast<float>(std::abs(value)));
            }
        }
    }
    return peak;
}

} // namespace

AudioLoudnessReport measure_audio_loudness_ebu_r128(const DecodedAudioAsset& asset,
                                                     bool retainWindows) {
    AudioLoudnessReport report;
    if (asset.metadata.channels == 0U || asset.metadata.channels > 2U ||
        asset.metadata.frameCount == 0U || asset.samples.empty()) return report;
    const std::vector<float> pcm = resample_to_48k(asset);
    if (pcm.empty()) return report;
    const std::uint8_t channels = asset.metadata.channels;
    const std::size_t frames = pcm.size() / channels;
    std::array<Biquad, 2> pre{pre_filter(), pre_filter()};
    std::array<Biquad, 2> rlb{rlb_filter(), rlb_filter()};
    std::vector<double> perFrameEnergy(frames, 0.0);
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        double sum{};
        for (std::uint8_t channel = 0U; channel < channels; ++channel) {
            const float sample = pcm[frame * channels + channel];
            report.samplePeak = std::max(report.samplePeak, std::abs(sample));
            const double weighted = rlb[channel].process(pre[channel].process(sample));
            sum += weighted * weighted;
        }
        perFrameEnergy[frame] = sum;
    }
    report.truePeak = estimate_true_peak(pcm, channels);
    report.truePeakDbtp = report.truePeak > 1.0e-12F
        ? 20.0F * std::log10(report.truePeak) : -120.0F;

    const auto momentaryEnergy = block_energies(perFrameEnergy, 19200U, 4800U);
    const auto shortEnergy = block_energies(perFrameEnergy, 144000U, 48000U);
    report.integratedLufs = integrated_loudness(momentaryEnergy, &report.gatedBlockCount);
    for (std::size_t i = 0U; i < momentaryEnergy.size(); ++i) {
        const float value = loudness_from_energy(momentaryEnergy[i]);
        report.maximumMomentaryLufs = std::max(report.maximumMomentaryLufs, value);
        if (retainWindows) report.momentary.push_back({static_cast<std::uint64_t>(i) * 4800U, value});
    }
    std::vector<float> lraValues;
    const float relativeLraGate = report.integratedLufs - 20.0F;
    for (std::size_t i = 0U; i < shortEnergy.size(); ++i) {
        const float value = loudness_from_energy(shortEnergy[i]);
        report.maximumShortTermLufs = std::max(report.maximumShortTermLufs, value);
        if (value >= -70.0F && value >= relativeLraGate) lraValues.push_back(value);
        if (retainWindows) report.shortTerm.push_back({static_cast<std::uint64_t>(i) * 48000U, value});
    }
    if (lraValues.size() >= 2U) report.loudnessRangeLu = percentile(lraValues, 0.95) - percentile(lraValues, 0.10);
    report.ebuR128Compatible = true;
    return report;
}

} // namespace dve::audio
