#include "dve/audio/audio_device_capture.hpp"

#include <algorithm>
#include <cmath>

namespace dve::audio {

AudioCaptureClockReconciler::AudioCaptureClockReconciler(std::uint32_t deviceSampleRate,
                                                         std::uint32_t engineSampleRate,
                                                         std::uint8_t channels,
                                                         std::size_t capacityFrames)
    : deviceSampleRate_(std::clamp(deviceSampleRate, 8000U, 384000U)),
      engineSampleRate_(std::clamp(engineSampleRate, 8000U, 384000U)),
      channels_(std::clamp<std::uint8_t>(channels, 1U, 2U)),
      capacityFrames_(std::max<std::size_t>(1024U, capacityFrames)),
      ring_(capacityFrames_ * channels_, 0.0F),
      activeRatio_(static_cast<double>(deviceSampleRate_) / static_cast<double>(engineSampleRate_)) {}

void AudioCaptureClockReconciler::reset() noexcept {
    writeFrame_.store(0U, std::memory_order_release);
    readFrame_.store(0U, std::memory_order_release);
    receivedFrames_.store(0U, std::memory_order_release);
    deliveredFrames_.store(0U, std::memory_order_release);
    overrunFrames_.store(0U, std::memory_order_release);
    underrunFrames_.store(0U, std::memory_order_release);
    phase_ = 0.0;
    activeRatio_.store(static_cast<double>(deviceSampleRate_) / static_cast<double>(engineSampleRate_), std::memory_order_relaxed);
    std::fill(ring_.begin(), ring_.end(), 0.0F);
}

std::size_t AudioCaptureClockReconciler::push_device_frames(std::span<const float> interleaved) noexcept {
    const std::size_t requested = interleaved.size() / channels_;
    if (requested == 0U) return 0U;
    std::uint64_t write = writeFrame_.load(std::memory_order_relaxed);
    const std::uint64_t read = readFrame_.load(std::memory_order_acquire);
    const std::uint64_t buffered = write - read;
    const std::size_t available = buffered >= capacityFrames_ ? 0U :
        capacityFrames_ - static_cast<std::size_t>(buffered);
    const std::size_t accepted = std::min(requested, available);
    for (std::size_t frame = 0U; frame < accepted; ++frame) {
        const std::size_t destinationFrame = static_cast<std::size_t>((write + frame) % capacityFrames_);
        for (std::uint8_t channel = 0U; channel < channels_; ++channel)
            ring_[destinationFrame * channels_ + channel] = interleaved[frame * channels_ + channel];
    }
    write += accepted;
    writeFrame_.store(write, std::memory_order_release);
    receivedFrames_.fetch_add(accepted, std::memory_order_relaxed);
    if (accepted < requested) overrunFrames_.fetch_add(requested - accepted, std::memory_order_relaxed);
    return accepted;
}

std::size_t AudioCaptureClockReconciler::drain_engine_frames(std::span<float> interleaved) noexcept {
    const std::size_t requested = interleaved.size() / channels_;
    if (requested == 0U) return 0U;
    std::fill(interleaved.begin(), interleaved.end(), 0.0F);
    std::uint64_t read = readFrame_.load(std::memory_order_relaxed);
    const std::uint64_t write = writeFrame_.load(std::memory_order_acquire);
    const std::uint64_t buffered = write - read;
    const double nominal = static_cast<double>(deviceSampleRate_) / static_cast<double>(engineSampleRate_);
    const double target = static_cast<double>(capacityFrames_) * 0.5;
    const double normalizedError = target > 1.0 ? (static_cast<double>(buffered) - target) / target : 0.0;
    const double correction = std::clamp(normalizedError * 0.0025, -0.01, 0.01);
    activeRatio_.store(nominal * (1.0 + correction), std::memory_order_relaxed);

    std::size_t produced{};
    while (produced < requested) {
        const std::uint64_t available = write - read;
        const std::uint64_t baseOffset = static_cast<std::uint64_t>(phase_);
        if (available <= baseOffset + 1U) break;
        const double fraction = phase_ - static_cast<double>(baseOffset);
        const std::size_t frameA = static_cast<std::size_t>((read + baseOffset) % capacityFrames_);
        const std::size_t frameB = static_cast<std::size_t>((read + baseOffset + 1U) % capacityFrames_);
        for (std::uint8_t channel = 0U; channel < channels_; ++channel) {
            const float a = ring_[frameA * channels_ + channel];
            const float b = ring_[frameB * channels_ + channel];
            interleaved[produced * channels_ + channel] =
                a + (b - a) * static_cast<float>(fraction);
        }
        phase_ += activeRatio_.load(std::memory_order_relaxed);
        const auto consumed = static_cast<std::uint64_t>(phase_);
        if (consumed > 0U) {
            read += consumed;
            phase_ -= static_cast<double>(consumed);
        }
        ++produced;
    }
    readFrame_.store(read, std::memory_order_release);
    deliveredFrames_.fetch_add(produced, std::memory_order_relaxed);
    if (produced < requested) underrunFrames_.fetch_add(requested - produced, std::memory_order_relaxed);
    return produced;
}

AudioCaptureClockTelemetry AudioCaptureClockReconciler::telemetry() const noexcept {
    AudioCaptureClockTelemetry result;
    result.deviceFramesReceived = receivedFrames_.load(std::memory_order_relaxed);
    result.engineFramesDelivered = deliveredFrames_.load(std::memory_order_relaxed);
    result.overrunFrames = overrunFrames_.load(std::memory_order_relaxed);
    result.underrunFrames = underrunFrames_.load(std::memory_order_relaxed);
    const std::uint64_t write = writeFrame_.load(std::memory_order_acquire);
    const std::uint64_t read = readFrame_.load(std::memory_order_acquire);
    result.bufferedDeviceFrames = static_cast<std::size_t>(std::min<std::uint64_t>(write - read, capacityFrames_));
    result.nominalRatio = static_cast<double>(deviceSampleRate_) / static_cast<double>(engineSampleRate_);
    result.activeRatio = activeRatio_.load(std::memory_order_relaxed);
    result.estimatedDriftPartsPerMillion = result.nominalRatio > 0.0
        ? (result.activeRatio / result.nominalRatio - 1.0) * 1.0e6 : 0.0;
    return result;
}

} // namespace dve::audio
