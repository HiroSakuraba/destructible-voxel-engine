// Audio-to-wavetable builder implementation.
#include "dve/audio/wavetable_builder.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace dve::audio {
namespace {

constexpr float kTwoPi = 6.283185307179586F;

// Remove DC offset and normalize to peak 0.9.
void remove_dc_and_normalize(std::vector<float>& audio) {
    if (audio.empty()) return;
    const double mean = std::accumulate(audio.begin(), audio.end(), 0.0) / audio.size();
    float peak = 0.0F;
    for (float& s : audio) {
        s = static_cast<float>(static_cast<double>(s) - mean);
        peak = std::max(peak, std::abs(s));
    }
    if (peak > 1e-6F) {
        const float gain = 0.9F / peak;
        for (float& s : audio) s *= gain;
    }
}

// Find zero-crossing positions (rising edge).
std::vector<std::size_t> find_zero_crossings(const std::vector<float>& audio) {
    std::vector<std::size_t> crossings;
    for (std::size_t i = 1; i < audio.size(); ++i) {
        if (audio[i-1] <= 0.0F && audio[i] > 0.0F)
            crossings.push_back(i);
    }
    return crossings;
}

// Correlation between two cycles (for phase alignment scoring).
float cycle_correlation(const float* a, const float* b, std::size_t n) noexcept {
    double sum = 0, normA = 0, normB = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(a[i]) * b[i];
        normA += static_cast<double>(a[i]) * a[i];
        normB += static_cast<double>(b[i]) * b[i];
    }
    const double denom = std::sqrt(normA * normB);
    return denom > 1e-9 ? static_cast<float>(sum / denom) : 0.0F;
}

} // namespace

float estimate_fundamental_frequency(
    const float* samples, std::size_t sampleCount, float sampleRate,
    float minHz, float maxHz) noexcept {
    if (!samples || sampleCount < 64 || sampleRate <= 0.0F) return 0.0F;

    const std::size_t minPeriod = static_cast<std::size_t>(sampleRate / maxHz);
    const std::size_t maxPeriod = static_cast<std::size_t>(sampleRate / minHz);
    if (minPeriod < 2 || maxPeriod <= minPeriod || maxPeriod >= sampleCount / 2)
        return 0.0F;

    // Autocorrelation.
    std::vector<float> corrs(maxPeriod + 1, 0.0F);
    float bestCorr = 0.0F;
    std::size_t bestPeriod = 0;
    const std::size_t window = std::min(sampleCount, maxPeriod * 4);
    for (std::size_t period = minPeriod; period <= maxPeriod; ++period) {
        double corr = 0, norm = 0;
        for (std::size_t i = 0; i + period < window; ++i) {
            corr += static_cast<double>(samples[i]) * samples[i + period];
            norm += static_cast<double>(samples[i]) * samples[i];
        }
        const float normalized = norm > 1e-9 ? static_cast<float>(corr / norm) : 0.0F;
        corrs[period] = normalized;
        // Prefer the smallest period with strong correlation (avoid octave errors).
        if (normalized > bestCorr + 0.05F) {
            bestCorr = normalized;
            bestPeriod = period;
        }
    }
    if (bestPeriod == 0 || bestCorr < 0.3F) return 0.0F;
    // Parabolic interpolation for sub-sample accuracy.
    float refinedPeriod = static_cast<float>(bestPeriod);
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod) {
        const float y0 = corrs[bestPeriod - 1];
        const float y1 = corrs[bestPeriod];
        const float y2 = corrs[bestPeriod + 1];
        const float denom = y0 - 2.0F * y1 + y2;
        if (std::abs(denom) > 1e-6F)
            refinedPeriod += 0.5F * (y0 - y2) / denom;
    }
    return sampleRate / refinedPeriod;
}

WavetableBuildResult build_wavetable_from_audio(
    const std::string& name,
    const float* samples, std::size_t sampleCount,
    const WavetableBuildOptions& options) {
    WavetableBuildResult result;

    if (!samples || sampleCount < 256) {
        result.error = "audio too short";
        return result;
    }

    // 1. Copy, remove DC, normalize.
    std::vector<float> audio(samples, samples + sampleCount);
    remove_dc_and_normalize(audio);

    // 2. Estimate fundamental.
    const float freq = estimate_fundamental_frequency(
        audio.data(), audio.size(), options.sampleRate,
        options.minFrequencyHertz, options.maxFrequencyHertz);
    if (freq <= 0.0F) {
        result.error = "could not estimate pitch";
        return result;
    }
    result.estimatedFrequencyHertz = freq;

    // 3. Find cycles via zero crossings.
    const auto crossings = find_zero_crossings(audio);
    if (crossings.size() < 4) {
        result.error = "not enough cycles found";
        return result;
    }

    const float periodSamples = options.sampleRate / freq;
    const std::size_t targetLen = kHQWavetableSamples;

    // 4. Extract and resample cycles.
    std::vector<std::vector<float>> cycles;
    for (std::size_t c = 0; c + 1 < crossings.size(); ++c) {
        const std::size_t start = crossings[c];
        const std::size_t end = crossings[c + 1];
        const std::size_t len = end - start;
        // Sanity: cycle length should be near the estimated period.
        if (len < periodSamples * 0.5F || len > periodSamples * 1.5F) continue;

        std::vector<float> cycle(targetLen);
        for (std::size_t i = 0; i < targetLen; ++i) {
            const float srcPos = static_cast<float>(i) / static_cast<float>(targetLen - 1) *
                                 static_cast<float>(len - 1);
            const std::size_t s0 = static_cast<std::size_t>(srcPos);
            const std::size_t s1 = std::min(s0 + 1, len - 1);
            const float frac = srcPos - static_cast<float>(s0);
            cycle[i] = audio[start + s0] * (1.0F - frac) + audio[start + s1] * frac;
        }
        cycles.push_back(std::move(cycle));
        if (cycles.size() >= options.targetFrames * 2) break; // gather extras for rejection
    }
    result.cyclesFound = cycles.size();
    if (cycles.empty()) {
        result.error = "no stable cycles extracted";
        return result;
    }

    // 5. Outlier rejection: compare each cycle to the median cycle.
    // Compute median cycle.
    std::vector<float> medianCycle(targetLen, 0.0F);
    for (std::size_t i = 0; i < targetLen; ++i) {
        std::vector<float> vals;
        vals.reserve(cycles.size());
        for (const auto& c : cycles) vals.push_back(c[i]);
        std::nth_element(vals.begin(), vals.begin() + vals.size()/2, vals.end());
        medianCycle[i] = vals[vals.size()/2];
    }
    // Score cycles by correlation to median.
    std::vector<std::pair<float, std::size_t>> scored;
    for (std::size_t i = 0; i < cycles.size(); ++i)
        scored.emplace_back(cycle_correlation(cycles[i].data(), medianCycle.data(), targetLen), i);
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Keep the best N.
    const std::size_t keepCount = std::min(options.targetFrames, scored.size());
    std::vector<std::vector<float>> keptFrames;
    keptFrames.reserve(keepCount);
    for (std::size_t i = 0; i < keepCount; ++i)
        keptFrames.push_back(cycles[scored[i].second]);
    result.cyclesKept = keptFrames.size();

    // 6. Cook the wavetable.
    result.table = cook_wavetable(name, keptFrames);
    result.success = true;
    return result;
}

} // namespace dve::audio
