// High-quality wavetable oscillator implementation.
#include "dve/audio/wavetable.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace dve::audio {
namespace {

constexpr float kTwoPi = 6.283185307179586F;

// Simple DFT for cooking (offline use only).
void forward_dft(const float* input, std::complex<float>* output, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<float> sum(0, 0);
        for (std::size_t t = 0; t < n; ++t) {
            const float angle = -kTwoPi * static_cast<float>(k * t) / static_cast<float>(n);
            sum += input[t] * std::complex<float>(std::cos(angle), std::sin(angle));
        }
        output[k] = sum;
    }
}

void inverse_dft(const std::complex<float>* input, float* output, std::size_t n) {
    for (std::size_t t = 0; t < n; ++t) {
        std::complex<float> sum(0, 0);
        for (std::size_t k = 0; k < n; ++k) {
            const float angle = kTwoPi * static_cast<float>(k * t) / static_cast<float>(n);
            sum += input[k] * std::complex<float>(std::cos(angle), std::sin(angle));
        }
        output[t] = sum.real() / static_cast<float>(n);
    }
}

// Cubic interpolation (Catmull-Rom).
float cubic_interp(float y0, float y1, float y2, float y3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5F * ((2.0F * y1) + (-y0 + y2) * t +
                   (2.0F * y0 - 5.0F * y1 + 4.0F * y2 - y3) * t2 +
                   (-y0 + 3.0F * y1 - 3.0F * y2 + y3) * t3);
}

std::uint64_t hash_frames(const std::vector<std::vector<float>>& frames) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& frame : frames) {
        for (float s : frame) {
            const auto bits = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(s));
            h ^= bits;
            h *= 1099511628211ULL;
        }
    }
    return h;
}

} // namespace

CookedWavetable::CookedWavetable() {
    samples.resize(kHQWavetableMips * kHQWavetableFrames * kHQWavetableSamples, 0.0F);
}

const float* CookedWavetable::frame_data(std::size_t mip, std::size_t frame) const noexcept {
    if (samples.empty()) return nullptr;
    mip = std::min(mip, kHQWavetableMips - 1);
    frame = std::min(frame, kHQWavetableFrames - 1);
    return samples.data() + (mip * kHQWavetableFrames + frame) * kHQWavetableSamples;
}

CookedWavetable cook_wavetable(const std::string& name,
                               const std::vector<std::vector<float>>& frames) {
    CookedWavetable table;
    table.name = name;
    table.contentHash = hash_frames(frames);

    const std::size_t nFrames = std::min(frames.size(), kHQWavetableFrames);
    const std::size_t n = kHQWavetableSamples;

    std::vector<std::complex<float>> spectrum(n);
    std::vector<float> filtered(n);
    std::vector<float> frameBuf(n);

    for (std::size_t mip = 0; mip < kHQWavetableMips; ++mip) {
        // Each mip halves the allowed harmonic content.
        const std::size_t maxHarmonic = n / 2 / (1U << mip);
        for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
            // Get source frame (wrap/clamp).
            const std::size_t srcFrame = std::min(f, nFrames - 1);
            const auto& src = frames[srcFrame];
            for (std::size_t i = 0; i < n; ++i)
                frameBuf[i] = i < src.size() ? src[i] : 0.0F;

            if (mip == 0) {
                // Full bandwidth: copy directly.
                float* dst = table.samples.data() + (mip * kHQWavetableFrames + f) * n;
                std::copy(frameBuf.begin(), frameBuf.end(), dst);
            } else {
                // Band-limit via DFT: zero harmonics above maxHarmonic.
                forward_dft(frameBuf.data(), spectrum.data(), n);
                for (std::size_t k = maxHarmonic + 1; k < n - maxHarmonic; ++k)
                    spectrum[k] = std::complex<float>(0, 0);
                inverse_dft(spectrum.data(), filtered.data(), n);
                float* dst = table.samples.data() + (mip * kHQWavetableFrames + f) * n;
                std::copy(filtered.begin(), filtered.end(), dst);
            }
        }
    }
    return table;
}

void cook_wavetable_inplace(CookedWavetable& table, const std::string& name,
                            const float* flatFrames, std::size_t nFrames,
                            std::vector<std::complex<float>>& spectrum,
                            std::vector<float>& filtered,
                            std::vector<float>& frameBuf) {
    table.name = name;
    const std::size_t n = kHQWavetableSamples;
    const std::size_t needed = kHQWavetableMips * kHQWavetableFrames * n;
    if (table.samples.size() < needed) table.samples.resize(needed, 0.0F);
    if (spectrum.size() < n) spectrum.resize(n);
    if (filtered.size() < n) filtered.resize(n, 0.0F);
    if (frameBuf.size() < n) frameBuf.resize(n, 0.0F);
    table.contentHash = 0;  // caller tracks content identity (see adopt_preset)
    nFrames = std::min(nFrames, kHQWavetableFrames);
    if (nFrames == 0) return;

    for (std::size_t mip = 0; mip < kHQWavetableMips; ++mip) {
        // Each mip halves the allowed harmonic content.
        const std::size_t maxHarmonic = n / 2 / (1U << mip);
        for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
            // Get source frame (wrap/clamp).
            const std::size_t srcFrame = std::min(f, nFrames - 1);
            const float* src = flatFrames + srcFrame * n;
            std::copy(src, src + n, frameBuf.begin());

            float* dst = table.samples.data() + (mip * kHQWavetableFrames + f) * n;
            if (mip == 0) {
                // Full bandwidth: copy directly.
                std::copy(frameBuf.begin(), frameBuf.end(), dst);
            } else {
                // Band-limit via DFT: zero harmonics above maxHarmonic.
                forward_dft(frameBuf.data(), spectrum.data(), n);
                for (std::size_t k = maxHarmonic + 1; k < n - maxHarmonic; ++k)
                    spectrum[k] = std::complex<float>(0, 0);
                inverse_dft(spectrum.data(), filtered.data(), n);
                std::copy(filtered.begin(), filtered.end(), dst);
            }
        }
    }
}

std::size_t wavetable_mip_for_frequency(float frequencyHertz, float sampleRate) noexcept {
    if (frequencyHertz <= 0.0F || sampleRate <= 0.0F) return 0;
    // Highest harmonic in mip 0 is ~256 (n/2). We want the highest harmonic
    // below Nyquist: mip = log2(freq * 256 / (sampleRate/2)).
    const float ratio = frequencyHertz * 256.0F / (sampleRate * 0.5F);
    if (ratio <= 1.0F) return 0;
    std::size_t mip = static_cast<std::size_t>(std::log2(ratio));
    return std::min(mip, kHQWavetableMips - 1);
}

float sample_wavetable(const CookedWavetable& table, float phase, float position,
                       std::size_t mip) noexcept {
    if (!table.valid()) return 0.0F;

    // Wrap phase (cyclic); clamp position to [0, 1] (first..last frame).
    // Wrapping position would turn exactly 1.0 into frame 0 while 0.999 reads
    // the last frame — an audible click on LFO->position sweeps and a wrong
    // frame for a static 1.0. This matches the legacy fallback path.
    phase -= std::floor(phase);
    position = std::clamp(position, 0.0F, 1.0F);

    // Frame interpolation (cubic across 4 frames).
    const float framePos = position * static_cast<float>(kHQWavetableFrames - 1);
    const std::size_t frameIdx = static_cast<std::size_t>(framePos);
    const float frameFrac = framePos - static_cast<float>(frameIdx);

    const std::size_t f0 = frameIdx > 0 ? frameIdx - 1 : 0;
    const std::size_t f1 = frameIdx;
    const std::size_t f2 = std::min(frameIdx + 1, kHQWavetableFrames - 1);
    const std::size_t f3 = std::min(frameIdx + 2, kHQWavetableFrames - 1);

    // Sample interpolation (cubic across 4 samples).
    const float samplePos = phase * static_cast<float>(kHQWavetableSamples);
    const std::size_t sIdx = static_cast<std::size_t>(samplePos);
    const float sFrac = samplePos - static_cast<float>(sIdx);

    auto sample_frame = [&](std::size_t frame) -> float {
        const float* data = table.frame_data(mip, frame);
        if (!data) return 0.0F;
        const std::size_t s0 = sIdx > 0 ? sIdx - 1 : kHQWavetableSamples - 1;
        const std::size_t s1 = sIdx % kHQWavetableSamples;
        const std::size_t s2 = (sIdx + 1) % kHQWavetableSamples;
        const std::size_t s3 = (sIdx + 2) % kHQWavetableSamples;
        return cubic_interp(data[s0], data[s1], data[s2], data[s3], sFrac);
    };

    const float y0 = sample_frame(f0);
    const float y1 = sample_frame(f1);
    const float y2 = sample_frame(f2);
    const float y3 = sample_frame(f3);
    return cubic_interp(y0, y1, y2, y3, frameFrac);
}

// --- Factory tables ---

CookedWavetable make_basic_morph_table() {
    std::vector<std::vector<float>> frames;
    frames.reserve(kHQWavetableFrames);
    const std::size_t n = kHQWavetableSamples;
    for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(kHQWavetableFrames - 1);
        std::vector<float> frame(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float phase = static_cast<float>(i) / static_cast<float>(n);
            // Morph: sine -> triangle -> saw -> square across the table.
            float sine = std::sin(kTwoPi * phase);
            float tri = 2.0F * std::abs(2.0F * phase - 1.0F) - 1.0F;
            float saw = 2.0F * phase - 1.0F;
            float sq = phase < 0.5F ? 1.0F : -1.0F;
            float v;
            if (t < 1.0F/3.0F) v = sine + (tri - sine) * (t * 3.0F);
            else if (t < 2.0F/3.0F) v = tri + (saw - tri) * ((t - 1.0F/3.0F) * 3.0F);
            else v = saw + (sq - saw) * ((t - 2.0F/3.0F) * 3.0F);
            frame[i] = v * 0.8F;
        }
        frames.push_back(std::move(frame));
    }
    return cook_wavetable("Basic Morph", frames);
}

CookedWavetable make_harmonic_series_table() {
    std::vector<std::vector<float>> frames;
    frames.reserve(kHQWavetableFrames);
    const std::size_t n = kHQWavetableSamples;
    for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(kHQWavetableFrames - 1);
        // Add harmonics progressively: frame f has harmonics up to (f+1).
        const int maxHarm = 1 + static_cast<int>(t * 32.0F);
        std::vector<float> frame(n, 0.0F);
        for (std::size_t i = 0; i < n; ++i) {
            const float phase = static_cast<float>(i) / static_cast<float>(n);
            float v = 0.0F;
            for (int h = 1; h <= maxHarm; ++h)
                v += std::sin(kTwoPi * h * phase) / static_cast<float>(h);
            frame[i] = v * 0.5F;
        }
        frames.push_back(std::move(frame));
    }
    return cook_wavetable("Harmonic Series", frames);
}

CookedWavetable make_formant_table() {
    std::vector<std::vector<float>> frames;
    frames.reserve(kHQWavetableFrames);
    const std::size_t n = kHQWavetableSamples;
    // Formant sweep: emphasize different harmonic bands.
    for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(kHQWavetableFrames - 1);
        const float centerHarm = 2.0F + t * 20.0F;
        std::vector<float> frame(n, 0.0F);
        for (std::size_t i = 0; i < n; ++i) {
            const float phase = static_cast<float>(i) / static_cast<float>(n);
            float v = 0.0F;
            for (int h = 1; h <= 32; ++h) {
                const float dist = std::abs(static_cast<float>(h) - centerHarm);
                const float weight = std::exp(-dist * dist / 8.0F);
                v += weight * std::sin(kTwoPi * h * phase) / std::sqrt(static_cast<float>(h));
            }
            frame[i] = v * 0.4F;
        }
        frames.push_back(std::move(frame));
    }
    return cook_wavetable("Formant Sweep", frames);
}

CookedWavetable make_digital_table() {
    std::vector<std::vector<float>> frames;
    frames.reserve(kHQWavetableFrames);
    const std::size_t n = kHQWavetableSamples;
    for (std::size_t f = 0; f < kHQWavetableFrames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(kHQWavetableFrames - 1);
        std::vector<float> frame(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float phase = static_cast<float>(i) / static_cast<float>(n);
            // Bit-crush and fold effects varying across the table.
            const int bits = 2 + static_cast<int>(t * 6.0F);
            const float levels = static_cast<float>(1 << bits);
            float v = std::sin(kTwoPi * phase) + 0.5F * std::sin(kTwoPi * 3.0F * phase + t * 4.0F);
            v = std::round(v * levels * 0.5F) / (levels * 0.5F);
            frame[i] = std::clamp(v, -1.0F, 1.0F) * 0.7F;
        }
        frames.push_back(std::move(frame));
    }
    return cook_wavetable("Digital", frames);
}

} // namespace dve::audio
