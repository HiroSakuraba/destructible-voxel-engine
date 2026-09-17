#include "dve/audio/audio_recorder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace dve::audio {

struct AudioTakeRecorder::Impl {
    Impl(std::uint32_t rate, std::uint8_t channelCount, std::size_t capacity)
        : sampleRate(std::clamp(rate, 8000U, 384000U)),
          channels(std::clamp<std::uint8_t>(channelCount, 1U, 2U)),
          ring(channels, std::max<std::size_t>(1024U, capacity)),
          worker([this](std::stop_token stop) { run(stop); }) {}

    std::uint32_t sampleRate{};
    std::uint8_t channels{};
    AudioStreamRing ring;
    std::atomic_bool armed{};
    std::atomic_bool workerDrained{true};
    std::atomic<std::uint64_t> captured{};
    std::atomic<std::uint64_t> committed{};
    std::atomic<std::uint64_t> dropped{};
    std::mutex mutex;
    std::condition_variable condition;
    std::string name{"Recorded take"};
    std::vector<float> samples;
    // Declared last so its destructor requests stop and joins before any state
    // used by run() is destroyed.
    std::jthread worker;

    void run(std::stop_token stop) {
        std::vector<float> buffer(4096U * channels);
        while (!stop.stop_requested()) {
            const std::size_t available = ring.available_read_frames();
            if (available == 0U) {
                if (!armed.load(std::memory_order_acquire)) {
                    workerDrained.store(true, std::memory_order_release);
                    condition.notify_all();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            workerDrained.store(false, std::memory_order_release);
            const std::size_t request = std::min<std::size_t>(available, 4096U);
            const std::size_t frames = ring.read(std::span<float>(buffer.data(), request * channels));
            if (frames == 0U) continue;
            {
                std::lock_guard lock(mutex);
                samples.insert(samples.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frames * channels));
            }
            committed.fetch_add(frames, std::memory_order_relaxed);
        }
    }
};

AudioTakeRecorder::AudioTakeRecorder(std::uint32_t sampleRate, std::uint8_t channels,
                                     std::size_t ringCapacityFrames)
    : impl_(std::make_unique<Impl>(sampleRate, channels, ringCapacityFrames)) {}
AudioTakeRecorder::~AudioTakeRecorder() = default;

bool AudioTakeRecorder::start(std::string name) noexcept {
    if (!impl_ || impl_->armed.load(std::memory_order_acquire)) return false;
    impl_->ring.reset();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->samples.clear();
        impl_->name = name.empty() ? "Recorded take" : std::move(name);
    }
    impl_->captured.store(0U, std::memory_order_relaxed);
    impl_->committed.store(0U, std::memory_order_relaxed);
    impl_->dropped.store(0U, std::memory_order_relaxed);
    impl_->workerDrained.store(false, std::memory_order_release);
    impl_->armed.store(true, std::memory_order_release);
    return true;
}

std::optional<DecodedAudioAsset> AudioTakeRecorder::stop(std::string* error) {
    if (!impl_ || !impl_->armed.exchange(false, std::memory_order_acq_rel)) {
        if (error) *error = "recorder is not running";
        return std::nullopt;
    }
    std::unique_lock lock(impl_->mutex);
    impl_->condition.wait_for(lock, std::chrono::seconds(5), [this] {
        return impl_->ring.available_read_frames() == 0U &&
               impl_->workerDrained.load(std::memory_order_acquire);
    });
    if (impl_->ring.available_read_frames() != 0U) {
        if (error) *error = "recorder worker did not drain before timeout";
        return std::nullopt;
    }
    DecodedAudioAsset result;
    result.metadata.name = impl_->name;
    result.metadata.sampleRate = impl_->sampleRate;
    result.metadata.channels = impl_->channels;
    result.metadata.storagePolicy = AudioStoragePolicy::Resident;
    result.samples = impl_->samples;
    lock.unlock();
    result.metadata.frameCount = result.samples.size() / impl_->channels;
    result.metadata.durationSeconds = static_cast<double>(result.metadata.frameCount) /
                                      static_cast<double>(impl_->sampleRate);
    double energy{};
    for (const float sample : result.samples) {
        result.metadata.peakLinear = std::max(result.metadata.peakLinear, std::abs(sample));
        energy += static_cast<double>(sample) * sample;
    }
    if (!result.samples.empty()) {
        result.metadata.rmsLinear = static_cast<float>(std::sqrt(energy / static_cast<double>(result.samples.size())));
        result.metadata.approximateLoudnessDbfs = result.metadata.rmsLinear > 1.0e-9F
            ? 20.0F * std::log10(result.metadata.rmsLinear) : -120.0F;
    }
    result.metadata.contentHash = audio_content_hash(result.samples, result.metadata);
    return result;
}

bool AudioTakeRecorder::recording() const noexcept {
    return impl_ && impl_->armed.load(std::memory_order_acquire);
}

AudioRecordingTelemetry AudioTakeRecorder::telemetry() const noexcept {
    AudioRecordingTelemetry result;
    if (!impl_) return result;
    result.capturedFrames = impl_->captured.load(std::memory_order_relaxed);
    result.committedFrames = impl_->committed.load(std::memory_order_relaxed);
    result.droppedFrames = impl_->dropped.load(std::memory_order_relaxed);
    result.bufferedFrames = impl_->ring.available_read_frames();
    result.recording = impl_->armed.load(std::memory_order_acquire);
    return result;
}

void AudioTakeRecorder::capture_interleaved(std::span<const float> interleaved,
                                            std::uint64_t firstFrame) noexcept {
    (void)firstFrame;
    if (!impl_ || !impl_->armed.load(std::memory_order_acquire)) return;
    const std::size_t requested = interleaved.size() / impl_->channels;
    const std::size_t written = impl_->ring.write(interleaved.first(requested * impl_->channels));
    impl_->captured.fetch_add(written, std::memory_order_relaxed);
    if (written < requested) impl_->dropped.fetch_add(requested - written, std::memory_order_relaxed);
}

} // namespace dve::audio
