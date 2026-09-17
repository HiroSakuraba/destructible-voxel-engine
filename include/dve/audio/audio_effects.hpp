#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dve::audio {

class IAudioEffectProcessor {
public:
    virtual ~IAudioEffectProcessor() = default;
    [[nodiscard]] virtual std::string_view identifier() const noexcept = 0;
    virtual void reset(std::uint32_t sampleRate, std::uint8_t channels) noexcept = 0;
    virtual void process(std::span<float> interleaved,
                         std::span<const float> sidechain = {}) noexcept = 0;
    [[nodiscard]] virtual std::vector<std::byte> save_state() const = 0;
    virtual bool load_state(std::span<const std::byte> state) noexcept = 0;
};

struct AudioEffectSlot {
    std::string identifier;
    std::uint32_t stateVersion{1U};
    float wetDry{1.0F};
    bool bypass{};
    bool missing{};
    std::vector<std::byte> state;
};

class AudioEffectRack {
public:
    explicit AudioEffectRack(std::uint32_t sampleRate = 48000U,
                             std::uint8_t channels = 2U,
                             std::size_t maximumBlockFrames = 4096U);
    bool add(std::unique_ptr<IAudioEffectProcessor> processor,
             float wetDry = 1.0F, bool bypass = false);
    bool add_missing(AudioEffectSlot placeholder);
    bool remove(std::size_t index) noexcept;
    bool set_bypass(std::size_t index, bool bypass) noexcept;
    bool set_wet_dry(std::size_t index, float wetDry) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<AudioEffectSlot> state() const;
    void process(std::span<float> interleaved, std::span<const float> sidechain = {}) noexcept;
private:
    struct Entry { AudioEffectSlot slot; std::unique_ptr<IAudioEffectProcessor> processor; };
    std::uint32_t sampleRate_{};
    std::uint8_t channels_{};
    std::size_t maximumBlockFrames_{};
    std::vector<Entry> entries_;
    std::vector<float> dryScratch_;
};

class GainAudioEffect final : public IAudioEffectProcessor {
public:
    explicit GainAudioEffect(float gain = 1.0F) : gain_(gain) {}
    [[nodiscard]] std::string_view identifier() const noexcept override { return "dve.gain"; }
    void reset(std::uint32_t, std::uint8_t) noexcept override {}
    void process(std::span<float> interleaved, std::span<const float> = {}) noexcept override;
    [[nodiscard]] std::vector<std::byte> save_state() const override;
    bool load_state(std::span<const std::byte> state) noexcept override;
    void set_gain(float value) noexcept;
private:
    float gain_{1.0F};
};

[[nodiscard]] bool deterministic_effect_render_check(IAudioEffectProcessor& processor,
                                                       std::span<const float> input,
                                                       std::uint32_t sampleRate,
                                                       std::uint8_t channels,
                                                       float tolerance = 1.0e-6F) noexcept;

} // namespace dve::audio
