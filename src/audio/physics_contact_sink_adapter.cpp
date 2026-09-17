#include "dve/audio/physics_contact_sink_adapter.hpp"

#include <cstdint>

namespace dve::audio {

static_assert((PhysicsContactAccumulatorSink::kQueueCapacity &
               (PhysicsContactAccumulatorSink::kQueueCapacity - 1U)) == 0U,
              "contact queue capacity must be a power of two");

PhysicsContactAccumulatorSink::PhysicsContactAccumulatorSink(
    PhysicsContactAudioAccumulator& accumulator) noexcept
    : accumulator_(&accumulator) {
    for (std::size_t index = 0; index < queue_.size(); ++index)
        queue_[index].sequence.store(index, std::memory_order_relaxed);
}

bool PhysicsContactAccumulatorSink::record_contact(
    const dve::PhysicsContactEvent& event) noexcept {
    std::size_t position = enqueuePosition_.load(std::memory_order_relaxed);
    for (;;) {
        QueueCell& cell = queue_[position & (kQueueCapacity - 1U)];
        const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
        const std::intptr_t difference = static_cast<std::intptr_t>(sequence) -
                                         static_cast<std::intptr_t>(position);
        if (difference == 0) {
            if (enqueuePosition_.compare_exchange_weak(position, position + 1U,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                cell.event = event;
                cell.sequence.store(position + 1U, std::memory_order_release);
                queuedContacts_.fetch_add(1U, std::memory_order_relaxed);
                return true;
            }
        } else if (difference < 0) {
            rejectedContacts_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        } else {
            position = enqueuePosition_.load(std::memory_order_relaxed);
        }
    }
}

bool PhysicsContactAccumulatorSink::try_dequeue(dve::PhysicsContactEvent& event) noexcept {
    QueueCell& cell = queue_[dequeuePosition_ & (kQueueCapacity - 1U)];
    const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
    const std::intptr_t difference = static_cast<std::intptr_t>(sequence) -
                                     static_cast<std::intptr_t>(dequeuePosition_ + 1U);
    if (difference != 0) return false;
    event = cell.event;
    cell.sequence.store(dequeuePosition_ + kQueueCapacity, std::memory_order_release);
    ++dequeuePosition_;
    return true;
}

void PhysicsContactAccumulatorSink::body_removed(std::uint64_t body) noexcept {
    (void)body;
    removedBodyNotifications_.fetch_add(1U, std::memory_order_relaxed);
}

std::size_t PhysicsContactAccumulatorSink::drain(
    double nowSeconds, std::vector<PhysicsContactAudio>& output, bool flushAll) {
    if (!accumulator_) return 0;
    std::size_t drained{};
    dve::PhysicsContactEvent event;
    while (try_dequeue(event)) {
        PhysicsContactSample sample;
        sample.bodyA = event.bodyA;
        sample.bodyB = event.bodyB;
        sample.materialA = event.materialA;
        sample.materialB = event.materialB;
        sample.position = {event.position.x, event.position.y, event.position.z};
        sample.relativeVelocity = {event.relativeVelocity.x, event.relativeVelocity.y,
                                   event.relativeVelocity.z};
        sample.contactNormal = {event.contactNormal.x, event.contactNormal.y,
                                event.contactNormal.z};
        sample.normalImpulse = event.normalImpulse;
        sample.effectiveMass = event.effectiveMass;
        sample.persistent = event.persistent;
        sample.timeSeconds = event.timeSeconds;
        (void)accumulator_->record(sample);
        ++drained;
    }
    drainedContacts_.fetch_add(drained, std::memory_order_relaxed);
    return accumulator_->drain(nowSeconds, output, flushAll);
}

PhysicsContactSinkMetrics PhysicsContactAccumulatorSink::metrics() const noexcept {
    return {queuedContacts_.load(std::memory_order_relaxed),
            rejectedContacts_.load(std::memory_order_relaxed),
            drainedContacts_.load(std::memory_order_relaxed),
            removedBodyNotifications_.load(std::memory_order_relaxed)};
}

} // namespace dve::audio
