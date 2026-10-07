#pragma once

#include <memory>
#include <string>

#include "dve/audio/mixer.hpp"
#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

struct SdlAudioDeviceOptions {
    std::uint32_t bufferFrames{}; // 0 keeps SDL's driver default; otherwise a request, not a guarantee.
};

struct SdlAudioDeviceStatus {
    std::uint32_t engineSampleRate{};
    std::uint32_t deviceSampleRate{};
    std::uint32_t requestedBufferFrames{};
    std::uint32_t deviceBufferFrames{};
    bool bufferHintAccepted{};
    bool deviceFormatKnown{};
};

class SdlSynthAudioDevice {
public:
    explicit SdlSynthAudioDevice(Synthesizer& synth, std::string* error = nullptr);
    explicit SdlSynthAudioDevice(AudioMixer& mixer, std::string* error = nullptr);
    SdlSynthAudioDevice(Synthesizer& synth, SdlAudioDeviceOptions options, std::string* error = nullptr);
    SdlSynthAudioDevice(AudioMixer& mixer, SdlAudioDeviceOptions options, std::string* error = nullptr);
    ~SdlSynthAudioDevice();
    SdlSynthAudioDevice(const SdlSynthAudioDevice&) = delete;
    SdlSynthAudioDevice& operator=(const SdlSynthAudioDevice&) = delete;

    // False when no audio driver/device could be opened (the constructor's error says why).
    // Callers should treat that as "run silently", not as a fatal error.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] SdlAudioDeviceStatus status() const noexcept;
    [[nodiscard]] std::string_view backend_name() const noexcept { return "SDL3 AudioStream"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::audio
