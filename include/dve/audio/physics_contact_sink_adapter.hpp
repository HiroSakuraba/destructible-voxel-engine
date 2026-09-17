#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dve/audio/physics_contact_audio_accumulator.hpp"
#include "dve/physics_contact_sink.hpp"

namespace dve::audio {

struct PhysicsContactSinkMetrics {
    std::uint64_t queuedContacts{};
    std::uint64_t rejectedContacts{};
    std::uint64_t drainedContacts{};
    std::uint64_t removedBodyNotifications{};
};

// Multi-producer, single-consumer bridge from solver callbacks to the audio accumulator.
// The callback path is bounded, lock-free, allocation-free, and does not touch the accumulator.
// drain() is called after PhysicsSystem::Update on the authority/audio-control thread.
class PhysicsContactAccumulatorSink final : public dve::IPhysicsContactSink {
public:
    static constexpr std::size_t kQueueCapacity = 1024;

    explicit PhysicsContactAccumulatorSink(PhysicsContactAudioAccumulator& accumulator) noexcept;

    bool record_contact(const dve::PhysicsContactEvent& event) noexcept override;
    void body_removed(std::uint64_t body) noexcept override;

    std::size_t drain(double nowSeconds, std::vector<PhysicsContactAudio>& output,
                      bool flushAll = false);
    [[nodiscard]] PhysicsContactSinkMetrics metrics() const noexcept;

private:
    struct QueueCell {
        std::atomic<std::size_t> sequence{};
        dve::PhysicsContactEvent event{};
    };

    [[nodiscard]] bool try_dequeue(dve::PhysicsContactEvent& event) noexcept;

    PhysicsContactAudioAccumulator* accumulator_{};
    std::array<QueueCell, kQueueCapacity> queue_{};
    std::atomic<std::size_t> enqueuePosition_{};
    std::size_t dequeuePosition_{};
    std::atomic<std::uint64_t> queuedContacts_{};
    std::atomic<std::uint64_t> rejectedContacts_{};
    std::atomic<std::uint64_t> drainedContacts_{};
    std::atomic<std::uint64_t> removedBodyNotifications_{};
};

} // namespace dve::audio
