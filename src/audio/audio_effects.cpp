#include "dve/audio/audio_effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dve::audio {

AudioEffectRack::AudioEffectRack(std::uint32_t sampleRate, std::uint8_t channels,
                                 std::size_t maximumBlockFrames)
    : sampleRate_(std::clamp(sampleRate, 8000U, 384000U)),
      channels_(std::clamp<std::uint8_t>(channels, 1U, 2U)),
      maximumBlockFrames_(std::max<std::size_t>(1U, maximumBlockFrames)),
      dryScratch_(maximumBlockFrames_ * channels_, 0.0F) {}

bool AudioEffectRack::add(std::unique_ptr<IAudioEffectProcessor> processor,
                          float wetDry, bool bypass) {
    if (!processor || entries_.size() >= 32U) return false;
    processor->reset(sampleRate_, channels_);
    AudioEffectSlot slot;
    slot.identifier = std::string(processor->identifier());
    slot.wetDry = std::clamp(wetDry, 0.0F, 1.0F);
    slot.bypass = bypass;
    slot.state = processor->save_state();
    entries_.push_back({std::move(slot), std::move(processor)});
    return true;
}

bool AudioEffectRack::add_missing(AudioEffectSlot placeholder) {
    if (entries_.size() >= 32U || placeholder.identifier.empty()) return false;
    placeholder.wetDry = std::clamp(placeholder.wetDry, 0.0F, 1.0F);
    placeholder.missing = true;
    entries_.push_back({std::move(placeholder), {}});
    return true;
}

bool AudioEffectRack::remove(std::size_t index) noexcept {
    if (index >= entries_.size()) return false;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool AudioEffectRack::set_bypass(std::size_t index, bool bypass) noexcept {
    if (index >= entries_.size()) return false;
    entries_[index].slot.bypass = bypass;
    return true;
}

bool AudioEffectRack::set_wet_dry(std::size_t index, float wetDry) noexcept {
    if (index >= entries_.size() || !std::isfinite(wetDry)) return false;
    entries_[index].slot.wetDry = std::clamp(wetDry, 0.0F, 1.0F);
    return true;
}

std::size_t AudioEffectRack::size() const noexcept { return entries_.size(); }

std::vector<AudioEffectSlot> AudioEffectRack::state() const {
    std::vector<AudioEffectSlot> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        auto slot = entry.slot;
        if (entry.processor) slot.state = entry.processor->save_state();
        result.push_back(std::move(slot));
    }
    return result;
}

void AudioEffectRack::process(std::span<float> interleaved,
                              std::span<const float> sidechain) noexcept {
    if (interleaved.empty() || interleaved.size() > dryScratch_.size()) return;
    for (auto& entry : entries_) {
        if (!entry.processor || entry.slot.missing || entry.slot.bypass) continue;
        const float wet = std::clamp(entry.slot.wetDry, 0.0F, 1.0F);
        if (wet < 1.0F) std::copy(interleaved.begin(), interleaved.end(), dryScratch_.begin());
        entry.processor->process(interleaved, sidechain);
        if (wet < 1.0F) {
            const float dry = 1.0F - wet;
            for (std::size_t i = 0U; i < interleaved.size(); ++i)
                interleaved[i] = dryScratch_[i] * dry + interleaved[i] * wet;
        }
    }
}

void GainAudioEffect::process(std::span<float> interleaved, std::span<const float>) noexcept {
    const float resolved = std::clamp(gain_, 0.0F, 16.0F);
    for (float& value : interleaved) value *= resolved;
}

std::vector<std::byte> GainAudioEffect::save_state() const {
    std::vector<std::byte> state(sizeof(float));
    std::memcpy(state.data(), &gain_, sizeof(float));
    return state;
}

bool GainAudioEffect::load_state(std::span<const std::byte> state) noexcept {
    if (state.size() != sizeof(float)) return false;
    float value{};
    std::memcpy(&value, state.data(), sizeof(float));
    if (!std::isfinite(value)) return false;
    gain_ = std::clamp(value, 0.0F, 16.0F);
    return true;
}

void GainAudioEffect::set_gain(float value) noexcept {
    if (std::isfinite(value)) gain_ = std::clamp(value, 0.0F, 16.0F);
}

bool deterministic_effect_render_check(IAudioEffectProcessor& processor,
                                       std::span<const float> input,
                                       std::uint32_t sampleRate,
                                       std::uint8_t channels,
                                       float tolerance) noexcept {
    if (channels == 0U || input.empty()) return false;
    std::vector<float> first(input.begin(), input.end());
    std::vector<float> second(input.begin(), input.end());
    const auto state = processor.save_state();
    processor.reset(sampleRate, channels);
    if (!processor.load_state(state)) return false;
    processor.process(first);
    processor.reset(sampleRate, channels);
    if (!processor.load_state(state)) return false;
    processor.process(second);
    tolerance = std::max(0.0F, tolerance);
    for (std::size_t i = 0U; i < first.size(); ++i) {
        if (!std::isfinite(first[i]) || !std::isfinite(second[i]) ||
            std::abs(first[i] - second[i]) > tolerance) return false;
    }
    return true;
}

} // namespace dve::audio
