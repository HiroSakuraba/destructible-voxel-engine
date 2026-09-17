#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dve/audio/audio_recorder.hpp"

namespace dve::audio {

struct AudioCaptureClockTelemetry {
    std::uint64_t deviceFramesReceived{};
    std::uint64_t engineFramesDelivered{};
    std::uint64_t overrunFrames{};
    std::uint64_t underrunFrames{};
    std::size_t bufferedDeviceFrames{};
    double nominalRatio{1.0};
    double activeRatio{1.0};
    double estimatedDriftPartsPerMillion{};
};

// Fixed-capacity single-producer/single-consumer input ring with bounded adaptive resampling.
// A device callback pushes raw frames; a worker drains engine-rate frames. The resampling ratio is
// corrected by at most one percent to keep the ring near its target fill under clock mismatch.
class AudioCaptureClockReconciler {
public:
    AudioCaptureClockReconciler(std::uint32_t deviceSampleRate,
                                std::uint32_t engineSampleRate,
                                std::uint8_t channels = 2U,
                                std::size_t capacityFrames = 65536U);
    AudioCaptureClockReconciler(const AudioCaptureClockReconciler&) = delete;
    AudioCaptureClockReconciler& operator=(const AudioCaptureClockReconciler&) = delete;

    void reset() noexcept;
    std::size_t push_device_frames(std::span<const float> interleaved) noexcept;
    std::size_t drain_engine_frames(std::span<float> interleaved) noexcept;
    [[nodiscard]] AudioCaptureClockTelemetry telemetry() const noexcept;
    [[nodiscard]] std::uint32_t device_sample_rate() const noexcept { return deviceSampleRate_; }
    [[nodiscard]] std::uint32_t engine_sample_rate() const noexcept { return engineSampleRate_; }
    [[nodiscard]] std::uint8_t channels() const noexcept { return channels_; }
private:
    std::uint32_t deviceSampleRate_{};
    std::uint32_t engineSampleRate_{};
    std::uint8_t channels_{};
    std::size_t capacityFrames_{};
    std::vector<float> ring_;
    std::atomic<std::uint64_t> writeFrame_{0U};
    std::atomic<std::uint64_t> readFrame_{0U};
    std::atomic<std::uint64_t> receivedFrames_{0U};
    std::atomic<std::uint64_t> deliveredFrames_{0U};
    std::atomic<std::uint64_t> overrunFrames_{0U};
    std::atomic<std::uint64_t> underrunFrames_{0U};
    double phase_{};
    std::atomic<double> activeRatio_{1.0};
};

struct AudioInputDeviceInfo {
    std::uint32_t id{};
    std::string name;
    std::uint32_t sampleRate{};
    std::uint8_t channels{};
    std::uint32_t bufferFrames{};
    bool systemDefault{};
};

struct AudioInputDeviceTelemetry {
    std::uint64_t callbackFrames{};
    std::uint64_t deliveredFrames{};
    std::uint64_t droppedFrames{};
    std::uint64_t callbackCalls{};
    std::size_t bufferedFrames{};
    double activeResampleRatio{1.0};
    bool running{};
};

// SDL3 recording-device adapter. Construction and device enumeration are control-thread operations.
// The SDL callback only pulls into fixed scratch storage and pushes the clock-reconciliation ring;
// a worker performs adaptive resampling and invokes the callback-safe capture sink.
class SdlAudioInputDevice {
public:
    static std::vector<AudioInputDeviceInfo> enumerate(std::string* error = nullptr);
    SdlAudioInputDevice(std::shared_ptr<IAudioCaptureSink> sink,
                        std::uint32_t engineSampleRate = 48000U,
                        std::uint8_t channels = 2U,
                        std::uint32_t deviceId = 0xFFFFFFFEU,
                        std::uint32_t requestedDeviceSampleRate = 0U,
                        std::string* error = nullptr);
    ~SdlAudioInputDevice();
    SdlAudioInputDevice(const SdlAudioInputDevice&) = delete;
    SdlAudioInputDevice& operator=(const SdlAudioInputDevice&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] AudioInputDeviceTelemetry telemetry() const noexcept;
    [[nodiscard]] std::string_view backend_name() const noexcept { return "SDL3 recording AudioStream"; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::audio
