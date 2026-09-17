#include "dve/audio/sdl_synth_audio_device.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>

namespace dve::audio {

struct SdlSynthAudioDevice::Impl {
    void* renderer{};
    void (*renderFunction)(void*, float*, std::size_t) noexcept{};
    std::uint32_t sampleRate{};
    SDL_AudioStream* stream{};
    std::array<float, 8192> scratch{}; // 4096 stereo frames, fixed-capacity callback storage

    static void SDLCALL callback(void* userdata, SDL_AudioStream* streamValue,
                                 int additionalAmount, int) noexcept {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || self->renderer == nullptr || self->renderFunction == nullptr ||
            streamValue == nullptr || additionalAmount <= 0) return;
        int remaining = additionalAmount;
        while (remaining > 0) {
            const int bytes = std::min<int>(remaining, static_cast<int>(self->scratch.size() * sizeof(float)));
            const std::size_t frames = static_cast<std::size_t>(bytes) / (2U * sizeof(float));
            if (frames == 0U) break;
            self->renderFunction(self->renderer, self->scratch.data(), frames);
            const int producedBytes = static_cast<int>(frames * 2U * sizeof(float));
            if (!SDL_PutAudioStreamData(streamValue, self->scratch.data(), producedBytes)) break;
            remaining -= producedBytes;
        }
    }

    bool open(std::string* error) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = static_cast<int>(sampleRate);
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::callback, this);
        if (stream == nullptr) {
            if (error) *error = SDL_GetError();
            return false;
        }
        if (!SDL_ResumeAudioStreamDevice(stream)) {
            if (error) *error = SDL_GetError();
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            return false;
        }
        return true;
    }
};

SdlSynthAudioDevice::SdlSynthAudioDevice(Synthesizer& synth, std::string* error)
    : impl_(std::make_unique<Impl>()) {
    impl_->renderer = &synth;
    impl_->sampleRate = synth.sample_rate();
    impl_->renderFunction = [](void* renderer, float* output, std::size_t frames) noexcept {
        static_cast<Synthesizer*>(renderer)->render(output, frames);
    };
    (void)impl_->open(error);
}

SdlSynthAudioDevice::SdlSynthAudioDevice(AudioMixer& mixer, std::string* error)
    : impl_(std::make_unique<Impl>()) {
    impl_->renderer = &mixer;
    impl_->sampleRate = mixer.sample_rate();
    impl_->renderFunction = [](void* renderer, float* output, std::size_t frames) noexcept {
        static_cast<AudioMixer*>(renderer)->render(output, frames);
    };
    (void)impl_->open(error);
}

SdlSynthAudioDevice::~SdlSynthAudioDevice() {
    if (impl_ && impl_->stream != nullptr) SDL_DestroyAudioStream(impl_->stream);
}

bool SdlSynthAudioDevice::valid() const noexcept { return impl_ && impl_->stream != nullptr; }

} // namespace dve::audio
