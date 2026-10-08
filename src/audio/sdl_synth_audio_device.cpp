#include "dve/audio/sdl_synth_audio_device.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>

namespace dve::audio {

struct SdlSynthAudioDevice::Impl {
    void* renderer{};
    void (*renderFunction)(void*, float*, std::size_t) noexcept{};
    std::uint32_t sampleRate{};
    std::uint64_t generation{};
    std::uint32_t latencyFrames{};
    void (*publishClock)(void*, AudioClockAnchor) noexcept{};
    std::uint64_t (*currentFrame)(void*) noexcept{};
    SdlAudioDeviceStatus status{};
    SDL_AudioStream* stream{};
    bool audioSubsystem{}; // this device's reference on SDL_INIT_AUDIO (SDL ref-counts it)
    std::array<float, 8192> scratch{}; // 4096 stereo frames, fixed-capacity callback storage

    static void SDLCALL callback(void* userdata, SDL_AudioStream* streamValue,
                                 int additionalAmount, int) noexcept {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || self->renderer == nullptr || self->renderFunction == nullptr ||
            streamValue == nullptr || additionalAmount <= 0) return;
        self->publishClock(self->renderer, {audio_host_nanoseconds(), self->currentFrame(self->renderer),
            self->generation, self->sampleRate, self->latencyFrames});
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
        // Take our own reference on the audio subsystem so the device works whether or not
        // the window host initialized audio, and fails cleanly (not fatally) when there is no
        // audio driver at all.
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            if (error) *error = SDL_GetError();
            return false;
        }
        audioSubsystem = true;
        static std::atomic<std::uint64_t> nextGeneration{1};
        generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = static_cast<int>(sampleRate);
        // Hints are global: scope the request to this open and restore the previous value
        // even on failure. The physical device may still choose a different buffer size.
        const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        const bool hadHint = hint != nullptr;
        const std::string previousHint = hadHint ? hint : "";
        if (status.requestedBufferFrames > 0U)
            status.bufferHintAccepted = SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
                                                    std::to_string(status.requestedBufferFrames).c_str());
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::callback, this);
        const std::string openError = stream == nullptr ? SDL_GetError() : "";
        if (status.requestedBufferFrames > 0U) {
            if (hadHint) (void)SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, previousHint.c_str());
            else (void)SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        }
        if (stream == nullptr) {
            if (error) *error = openError;
            release_subsystem();
            return false;
        }
        SDL_AudioSpec physicalSpec{};
        int frames{};
        const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
        status.deviceFormatKnown = device != 0U && SDL_GetAudioDeviceFormat(device, &physicalSpec, &frames);
        if (status.deviceFormatKnown) {
            status.deviceSampleRate = static_cast<std::uint32_t>(std::max(0, physicalSpec.freq));
            status.deviceBufferFrames = static_cast<std::uint32_t>(std::max(0, frames));
        }
        latencyFrames = status.deviceSampleRate ? static_cast<std::uint32_t>(
            std::uint64_t(status.deviceBufferFrames) * sampleRate / status.deviceSampleRate) : 0U;
        publishClock(renderer,{audio_host_nanoseconds(),currentFrame(renderer),generation,sampleRate,latencyFrames});
        if (!SDL_ResumeAudioStreamDevice(stream)) {
            if (error) *error = SDL_GetError();
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            release_subsystem();
            return false;
        }
        return true;
    }

    void release_subsystem() noexcept {
        if (audioSubsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audioSubsystem = false;
    }
};

SdlSynthAudioDevice::SdlSynthAudioDevice(Synthesizer& synth, std::string* error)
    : SdlSynthAudioDevice(synth, SdlAudioDeviceOptions{}, error) {}

SdlSynthAudioDevice::SdlSynthAudioDevice(Synthesizer& synth, SdlAudioDeviceOptions options, std::string* error)
    : impl_(std::make_unique<Impl>()) {
    impl_->renderer = &synth;
    impl_->sampleRate = synth.sample_rate();
    impl_->status.engineSampleRate = impl_->sampleRate;
    impl_->status.requestedBufferFrames = options.bufferFrames;
    impl_->renderFunction = [](void* renderer, float* output, std::size_t frames) noexcept {
        static_cast<Synthesizer*>(renderer)->render(output, frames);
    };
    impl_->publishClock = [](void* renderer, AudioClockAnchor a) noexcept { static_cast<Synthesizer*>(renderer)->publish_audio_clock(a); };
    impl_->currentFrame = [](void* renderer) noexcept { return static_cast<Synthesizer*>(renderer)->current_frame(); };
    (void)impl_->open(error);
}

SdlSynthAudioDevice::SdlSynthAudioDevice(AudioMixer& mixer, std::string* error)
    : SdlSynthAudioDevice(mixer, SdlAudioDeviceOptions{}, error) {}

SdlSynthAudioDevice::SdlSynthAudioDevice(AudioMixer& mixer, SdlAudioDeviceOptions options, std::string* error)
    : impl_(std::make_unique<Impl>()) {
    impl_->renderer = &mixer;
    impl_->sampleRate = mixer.sample_rate();
    impl_->status.engineSampleRate = impl_->sampleRate;
    impl_->status.requestedBufferFrames = options.bufferFrames;
    impl_->renderFunction = [](void* renderer, float* output, std::size_t frames) noexcept {
        static_cast<AudioMixer*>(renderer)->render(output, frames);
    };
    impl_->publishClock = [](void* renderer, AudioClockAnchor a) noexcept { static_cast<AudioMixer*>(renderer)->publish_audio_clock(a); };
    impl_->currentFrame = [](void* renderer) noexcept { return static_cast<AudioMixer*>(renderer)->current_frame(); };
    (void)impl_->open(error);
}

SdlSynthAudioDevice::~SdlSynthAudioDevice() {
    if (!impl_) return;
    if (impl_->stream != nullptr) SDL_DestroyAudioStream(impl_->stream);
    impl_->stream = nullptr;
    impl_->release_subsystem();
}

bool SdlSynthAudioDevice::valid() const noexcept { return impl_ && impl_->stream != nullptr; }
SdlAudioDeviceStatus SdlSynthAudioDevice::status() const noexcept { return impl_ ? impl_->status : SdlAudioDeviceStatus{}; }

} // namespace dve::audio
