#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

class IAudioCaptureSink {
public:
    virtual ~IAudioCaptureSink() = default;
    // Called from the audio callback. Implementations must not allocate, lock, log, or perform I/O.
    virtual void capture_interleaved(std::span<const float> interleaved,
                                     std::uint64_t firstFrame) noexcept = 0;
};

struct AudioRecordingTelemetry {
    std::uint64_t capturedFrames{};
    std::uint64_t committedFrames{};
    std::uint64_t droppedFrames{};
    std::size_t bufferedFrames{};
    bool recording{};
};

// Callback-safe recorder. The callback writes to a fixed single-producer/single-consumer ring;
// a worker drains it into an editable take. stop() is a control-thread operation and returns the
// completed canonical PCM asset.
class AudioTakeRecorder final : public IAudioCaptureSink {
public:
    explicit AudioTakeRecorder(std::uint32_t sampleRate = 48000,
                               std::uint8_t channels = 2,
                               std::size_t ringCapacityFrames = 65536);
    ~AudioTakeRecorder() override;
    AudioTakeRecorder(const AudioTakeRecorder&) = delete;
    AudioTakeRecorder& operator=(const AudioTakeRecorder&) = delete;

    bool start(std::string name = "Recorded take") noexcept;
    [[nodiscard]] std::optional<DecodedAudioAsset> stop(std::string* error = nullptr);
    [[nodiscard]] bool recording() const noexcept;
    [[nodiscard]] AudioRecordingTelemetry telemetry() const noexcept;
    void capture_interleaved(std::span<const float> interleaved,
                             std::uint64_t firstFrame) noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::audio
