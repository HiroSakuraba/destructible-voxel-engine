#include "dve/audio/audio_device_capture.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace dve::audio {

struct SdlAudioInputDevice::Impl {
    std::shared_ptr<IAudioCaptureSink> sink;
    std::uint32_t engineSampleRate{};
    std::uint8_t channels{};
    SDL_AudioStream* stream{};
    std::unique_ptr<AudioCaptureClockReconciler> reconciler;
    std::jthread worker;
    std::array<float, 8192> callbackScratch{};
    std::atomic<std::uint64_t> callbackFrames{0U};
    std::atomic<std::uint64_t> callbackCalls{0U};
    std::atomic<std::uint64_t> droppedFrames{0U};
    std::atomic<bool> running{false};

    static void SDLCALL callback(void* userdata, SDL_AudioStream* streamValue,
                                 int additionalAmount, int) noexcept {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || streamValue == nullptr || self->reconciler == nullptr ||
            additionalAmount <= 0) return;
        self->callbackCalls.fetch_add(1U, std::memory_order_relaxed);
        int remaining = additionalAmount;
        const int frameBytes = static_cast<int>(self->channels * sizeof(float));
        while (remaining >= frameBytes) {
            const int maximumBytes = static_cast<int>(self->callbackScratch.size() * sizeof(float));
            const int requestedBytes = std::min(remaining, maximumBytes - maximumBytes % frameBytes);
            const int receivedBytes = SDL_GetAudioStreamData(streamValue, self->callbackScratch.data(), requestedBytes);
            if (receivedBytes <= 0) break;
            const std::size_t frames = static_cast<std::size_t>(receivedBytes / frameBytes);
            const std::size_t accepted = self->reconciler->push_device_frames(
                std::span<const float>(self->callbackScratch.data(), frames * self->channels));
            self->callbackFrames.fetch_add(frames, std::memory_order_relaxed);
            if (accepted < frames) self->droppedFrames.fetch_add(frames - accepted, std::memory_order_relaxed);
            remaining -= receivedBytes;
        }
    }

    void start_worker() {
        running.store(true, std::memory_order_release);
        worker = std::jthread([this](std::stop_token stopToken) {
            std::array<float, 1024> block{}; // 512 stereo or 1024 mono frames
            const std::size_t framesPerBlock = block.size() / channels;
            std::uint64_t firstFrame{};
            while (!stopToken.stop_requested()) {
                const std::size_t produced = reconciler->drain_engine_frames(
                    std::span<float>(block.data(), framesPerBlock * channels));
                if (produced > 0U && sink) {
                    sink->capture_interleaved(
                        std::span<const float>(block.data(), produced * channels), firstFrame);
                    firstFrame += produced;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            running.store(false, std::memory_order_release);
        });
    }

    void shutdown() noexcept {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        if (stream != nullptr) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        running.store(false, std::memory_order_release);
    }
};

std::vector<AudioInputDeviceInfo> SdlAudioInputDevice::enumerate(std::string* error) {
    std::vector<AudioInputDeviceInfo> result;
    int count{};
    SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
    if (devices == nullptr) {
        if (error != nullptr) *error = SDL_GetError();
        return result;
    }
    result.reserve(static_cast<std::size_t>(std::max(0, count)) + 1U);
    SDL_AudioSpec defaultSpec{};
    int defaultFrames{};
    const char* defaultName = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &defaultSpec, &defaultFrames)) {
        result.push_back({static_cast<std::uint32_t>(SDL_AUDIO_DEVICE_DEFAULT_RECORDING),
                          defaultName != nullptr ? std::string("System default (currently ") + defaultName + ")"
                                                 : "System default recording device",
                          static_cast<std::uint32_t>(std::max(0, defaultSpec.freq)),
                          static_cast<std::uint8_t>(std::clamp(defaultSpec.channels, 0, 255)),
                          static_cast<std::uint32_t>(std::max(0, defaultFrames)), true});
    }
    for (int index = 0; index < count; ++index) {
        SDL_AudioSpec spec{};
        int frames{};
        const char* name = SDL_GetAudioDeviceName(devices[index]);
        (void)SDL_GetAudioDeviceFormat(devices[index], &spec, &frames);
        result.push_back({static_cast<std::uint32_t>(devices[index]), name != nullptr ? name : "Recording device",
                          static_cast<std::uint32_t>(std::max(0, spec.freq)),
                          static_cast<std::uint8_t>(std::clamp(spec.channels, 0, 255)),
                          static_cast<std::uint32_t>(std::max(0, frames)), false});
    }
    SDL_free(devices);
    return result;
}

SdlAudioInputDevice::SdlAudioInputDevice(std::shared_ptr<IAudioCaptureSink> sink,
                                         std::uint32_t engineSampleRate,
                                         std::uint8_t channels,
                                         std::uint32_t deviceId,
                                         std::uint32_t requestedDeviceSampleRate,
                                         std::string* error)
    : impl_(std::make_unique<Impl>()) {
    impl_->sink = std::move(sink);
    impl_->engineSampleRate = std::clamp(engineSampleRate, 8000U, 384000U);
    impl_->channels = std::clamp<std::uint8_t>(channels, 1U, 2U);
    const SDL_AudioDeviceID resolvedDevice = static_cast<SDL_AudioDeviceID>(deviceId);
    SDL_AudioSpec physical{};
    int physicalFrames{};
    if (!SDL_GetAudioDeviceFormat(resolvedDevice, &physical, &physicalFrames)) {
        physical.freq = static_cast<int>(impl_->engineSampleRate);
        physical.channels = impl_->channels;
        physical.format = SDL_AUDIO_F32;
    }
    const std::uint32_t deviceRate = requestedDeviceSampleRate != 0U
        ? std::clamp(requestedDeviceSampleRate, 8000U, 384000U)
        : static_cast<std::uint32_t>(physical.freq > 0 ? physical.freq : static_cast<int>(impl_->engineSampleRate));
    SDL_AudioSpec appSpec{};
    appSpec.format = SDL_AUDIO_F32;
    appSpec.channels = impl_->channels;
    appSpec.freq = static_cast<int>(deviceRate);
    impl_->reconciler = std::make_unique<AudioCaptureClockReconciler>(
        deviceRate, impl_->engineSampleRate, impl_->channels, 65536U);
    impl_->stream = SDL_OpenAudioDeviceStream(resolvedDevice, &appSpec, &Impl::callback, impl_.get());
    if (impl_->stream == nullptr) {
        if (error != nullptr) *error = SDL_GetError();
        return;
    }
    impl_->start_worker();
    if (!SDL_ResumeAudioStreamDevice(impl_->stream)) {
        if (error != nullptr) *error = SDL_GetError();
        impl_->shutdown();
    }
}

SdlAudioInputDevice::~SdlAudioInputDevice() {
    if (impl_) impl_->shutdown();
}

bool SdlAudioInputDevice::valid() const noexcept {
    return impl_ && impl_->stream != nullptr && impl_->reconciler != nullptr;
}

AudioInputDeviceTelemetry SdlAudioInputDevice::telemetry() const noexcept {
    AudioInputDeviceTelemetry result;
    if (!impl_) return result;
    result.callbackFrames = impl_->callbackFrames.load(std::memory_order_relaxed);
    result.callbackCalls = impl_->callbackCalls.load(std::memory_order_relaxed);
    result.droppedFrames = impl_->droppedFrames.load(std::memory_order_relaxed);
    result.running = impl_->running.load(std::memory_order_acquire);
    if (impl_->reconciler) {
        const auto clock = impl_->reconciler->telemetry();
        result.deliveredFrames = clock.engineFramesDelivered;
        result.bufferedFrames = clock.bufferedDeviceFrames;
        result.activeResampleRatio = clock.activeRatio;
        result.droppedFrames += clock.overrunFrames;
    }
    return result;
}

} // namespace dve::audio
