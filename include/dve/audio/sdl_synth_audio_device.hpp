#pragma once

#include <memory>
#include <string>

#include "dve/audio/mixer.hpp"
#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

class SdlSynthAudioDevice {
public:
    explicit SdlSynthAudioDevice(Synthesizer& synth, std::string* error = nullptr);
    explicit SdlSynthAudioDevice(AudioMixer& mixer, std::string* error = nullptr);
    ~SdlSynthAudioDevice();
    SdlSynthAudioDevice(const SdlSynthAudioDevice&) = delete;
    SdlSynthAudioDevice& operator=(const SdlSynthAudioDevice&) = delete;

    // False when no audio driver/device could be opened (the constructor's error says why).
    // Callers should treat that as "run silently", not as a fatal error.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string_view backend_name() const noexcept { return "SDL3 AudioStream"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::audio
