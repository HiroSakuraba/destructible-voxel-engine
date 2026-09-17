#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dve::audio {

enum class AudioStoragePolicy : std::uint8_t { Resident, Streamed };

struct AudioLoopRegion {
    std::uint64_t beginFrame{};
    std::uint64_t endFrame{}; // exclusive
    bool enabled{};
};

struct AudioCueMarker {
    std::uint64_t frame{};
    std::string name;
};

struct AudioAssetMetadata {
    std::uint32_t version{1};
    std::string name;
    std::uint32_t sampleRate{48000};
    std::uint8_t channels{2};
    std::uint64_t frameCount{};
    double durationSeconds{};
    float peakLinear{};
    float rmsLinear{};
    float approximateLoudnessDbfs{-120.0F};
    AudioStoragePolicy storagePolicy{AudioStoragePolicy::Resident};
    AudioLoopRegion loop{};
    std::vector<AudioCueMarker> cues;
    std::uint64_t contentHash{};
};

struct DecodedAudioAsset {
    AudioAssetMetadata metadata;
    std::vector<float> samples;
};

struct AudioImportOptions {
    std::uint32_t targetSampleRate{48000};
    AudioStoragePolicy storagePolicy{AudioStoragePolicy::Resident};
    bool normalize{};
    float normalizePeak{0.95F};
};

struct AudioImportCapabilities {
    bool nativeWav{};
    bool sndfile{};
    bool ffmpeg{};
};

[[nodiscard]] AudioImportCapabilities audio_import_capabilities() noexcept;

// Imports RIFF/WAVE PCM (8/16/24/32-bit) and IEEE-float (32-bit). The cooker converts to
// interleaved finite float samples and optionally resamples to the canonical engine rate.
// General import entry point. Native WAV parsing is always available. Optional libsndfile adds
// sampled-audio formats including FLAC, Ogg Vorbis, AIFF, and MP3 on current libsndfile builds.
// Optional FFmpeg authoring support extracts and decodes the first audio stream from broad media
// containers such as MP4/M4A, MOV, WebM, and WMA. All decoding occurs off the audio callback.
[[nodiscard]] std::optional<DecodedAudioAsset> import_audio_file(
    const std::filesystem::path& path, const AudioImportOptions& options = {},
    std::string* error = nullptr);

[[nodiscard]] std::optional<DecodedAudioAsset> import_wav_file(
    const std::filesystem::path& path, const AudioImportOptions& options = {},
    std::string* error = nullptr);

// Writes an editable/exportable RIFF/WAVE file. Float32 preserves the engine mix without an
// additional quantization step; PCM16 is intended for broad external compatibility.
enum class WavSampleEncoding : std::uint8_t { Float32, Pcm16 };
[[nodiscard]] bool write_wav_file(const std::filesystem::path& path,
                                  const DecodedAudioAsset& asset,
                                  WavSampleEncoding encoding = WavSampleEncoding::Float32,
                                  std::string* error = nullptr);

// Versioned source-control-independent cooked sample format. It stores deterministic metadata,
// cue/loop information, and interleaved float PCM. Streamed assets use the same file and are read
// incrementally by CookedAudioStream rather than copied into the callback-visible resident bank.
[[nodiscard]] bool write_cooked_audio_asset(const std::filesystem::path& path,
                                             const DecodedAudioAsset& asset,
                                             std::string* error = nullptr);
[[nodiscard]] std::optional<DecodedAudioAsset> read_cooked_audio_asset(
    const std::filesystem::path& path, std::string* error = nullptr);
[[nodiscard]] std::optional<AudioAssetMetadata> read_cooked_audio_metadata(
    const std::filesystem::path& path, std::string* error = nullptr);

struct AudioStreamTelemetry {
    std::uint64_t producedFrames{};
    std::uint64_t consumedFrames{};
    std::uint64_t underrunFrames{};
    std::uint64_t loopCount{};
    std::size_t bufferedFrames{};
    std::size_t capacityFrames{};
    bool endOfStream{};
    bool running{};
};

// Single-producer/single-consumer interleaved frame ring. Storage is allocated on the control
// thread. read()/write() are lock-free and suitable for worker-to-audio-callback publication.
class AudioStreamRing {
public:
    AudioStreamRing(std::uint8_t channels, std::size_t capacityFrames);
    [[nodiscard]] std::uint8_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacityFrames_; }
    [[nodiscard]] std::size_t available_read_frames() const noexcept;
    [[nodiscard]] std::size_t available_write_frames() const noexcept;
    std::size_t write(std::span<const float> interleaved) noexcept;
    std::size_t read(std::span<float> interleaved) noexcept;
    void reset() noexcept;
private:
    std::uint8_t channels_{};
    std::size_t capacityFrames_{};
    std::vector<float> data_;
    alignas(64) std::atomic<std::uint64_t> writeFrame_{};
    alignas(64) std::atomic<std::uint64_t> readFrame_{};
};

// Background decoder for the deterministic .dvesample format. This is intended for music,
// ambience, and long dialogue. The worker performs file I/O; the callback only drains the ring.
class CookedAudioStream {
public:
    explicit CookedAudioStream(const std::filesystem::path& path,
                               std::size_t ringCapacityFrames = 16384,
                               std::string* error = nullptr);
    ~CookedAudioStream();
    CookedAudioStream(const CookedAudioStream&) = delete;
    CookedAudioStream& operator=(const CookedAudioStream&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AudioAssetMetadata& metadata() const noexcept;
    void set_looping(bool enabled) noexcept;
    void request_rewind() noexcept;
    void set_running(bool running) noexcept;

    // Returns frames actually read and clears the remainder to silence. No locks, allocation,
    // logging, or file access occur here.
    std::size_t read(float* interleaved, std::size_t frameCount) noexcept;
    [[nodiscard]] AudioStreamTelemetry telemetry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::uint64_t audio_content_hash(std::span<const float> samples,
                                                const AudioAssetMetadata& metadata) noexcept;

} // namespace dve::audio
