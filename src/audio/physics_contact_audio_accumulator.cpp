#include "dve/audio/physics_contact_audio_accumulator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::audio {
namespace {

bool finite_vec(AudioVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float dot(AudioVec3 a, AudioVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

AudioVec3 normalized(AudioVec3 value) noexcept {
    const float lengthSquared = dot(value, value);
    if (!(lengthSquared > 1.0e-12F) || !std::isfinite(lengthSquared)) return {0.0F, 1.0F, 0.0F};
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
}

std::int32_t spatial_cell(float coordinate, float cellSize) noexcept {
    const double value = std::floor(static_cast<double>(coordinate) /
                                    static_cast<double>(cellSize));
    const double minimum = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    const double maximum = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    return static_cast<std::int32_t>(std::clamp(value, minimum, maximum));
}

} // namespace

PhysicsContactAudioAccumulator::PhysicsContactAudioAccumulator(
    PhysicsContactAccumulatorSettings settings) noexcept
    : settings_(settings) {
    if (!(settings_.spatialCellMeters > 0.0F) || !std::isfinite(settings_.spatialCellMeters))
        settings_.spatialCellMeters = 0.5F;
    if (!(settings_.aggregationWindowSeconds > 0.0) ||
        !std::isfinite(settings_.aggregationWindowSeconds))
        settings_.aggregationWindowSeconds = 0.08;
    settings_.capacity = std::clamp<std::size_t>(settings_.capacity, 1U, kMaximumBuckets);
}

bool PhysicsContactAudioAccumulator::valid_sample(const PhysicsContactSample& sample) const noexcept {
    return sample.bodyA != sample.bodyB && finite_vec(sample.position) &&
           finite_vec(sample.relativeVelocity) && finite_vec(sample.contactNormal) &&
           std::isfinite(sample.normalImpulse) && std::isfinite(sample.effectiveMass) &&
           sample.effectiveMass >= 0.0F && std::isfinite(sample.timeSeconds);
}

PhysicsContactAudioAccumulator::Bucket* PhysicsContactAudioAccumulator::find_or_create(
    const PhysicsContactSample& sample, std::int32_t cellX, std::int32_t cellY,
    std::int32_t cellZ, std::int64_t window) noexcept {
    const bool swapBodies = sample.bodyB < sample.bodyA;
    const std::uint64_t bodyA = swapBodies ? sample.bodyB : sample.bodyA;
    const std::uint64_t bodyB = swapBodies ? sample.bodyA : sample.bodyB;
    const std::uint16_t materialA = swapBodies ? sample.materialB : sample.materialA;
    const std::uint16_t materialB = swapBodies ? sample.materialA : sample.materialB;

    Bucket* freeBucket = nullptr;
    for (std::size_t index = 0; index < settings_.capacity; ++index) {
        Bucket& bucket = buckets_[index];
        if (!bucket.active) {
            if (!freeBucket) freeBucket = &bucket;
            continue;
        }
        if (bucket.bodyA == bodyA && bucket.bodyB == bodyB &&
            bucket.materialA == materialA && bucket.materialB == materialB &&
            bucket.cellX == cellX && bucket.cellY == cellY && bucket.cellZ == cellZ &&
            bucket.window == window) return &bucket;
    }
    if (!freeBucket) return nullptr;
    *freeBucket = {};
    freeBucket->active = true;
    freeBucket->bodyA = bodyA;
    freeBucket->bodyB = bodyB;
    freeBucket->materialA = materialA;
    freeBucket->materialB = materialB;
    freeBucket->cellX = cellX;
    freeBucket->cellY = cellY;
    freeBucket->cellZ = cellZ;
    freeBucket->window = window;
    ++metrics_.activeBuckets;
    return freeBucket;
}

bool PhysicsContactAudioAccumulator::record(const PhysicsContactSample& sample) noexcept {
    ++metrics_.submittedSamples;
    if (!valid_sample(sample)) {
        ++metrics_.rejectedSamples;
        return false;
    }
    const std::int32_t cellX = spatial_cell(sample.position.x, settings_.spatialCellMeters);
    const std::int32_t cellY = spatial_cell(sample.position.y, settings_.spatialCellMeters);
    const std::int32_t cellZ = spatial_cell(sample.position.z, settings_.spatialCellMeters);
    const double rawWindow = std::floor(sample.timeSeconds / settings_.aggregationWindowSeconds);
    const double minimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    const double maximum = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    const std::int64_t window = static_cast<std::int64_t>(std::clamp(rawWindow, minimum, maximum));
    Bucket* bucket = find_or_create(sample, cellX, cellY, cellZ, window);
    if (!bucket) {
        ++metrics_.rejectedSamples;
        return false;
    }

    float impulse = std::max(0.0F, sample.normalImpulse);
    if (!(impulse > 0.0F) && sample.effectiveMass > 0.0F) {
        const AudioVec3 normal = normalized(sample.contactNormal);
        impulse = std::abs(dot(sample.relativeVelocity, normal)) * sample.effectiveMass;
        if (impulse > 0.0F) ++metrics_.estimatedImpulses;
    }
    const float weight = std::max(impulse, 1.0e-4F);
    bucket->weightedPosition.x += sample.position.x * weight;
    bucket->weightedPosition.y += sample.position.y * weight;
    bucket->weightedPosition.z += sample.position.z * weight;
    bucket->weightedVelocity.x += sample.relativeVelocity.x * weight;
    bucket->weightedVelocity.y += sample.relativeVelocity.y * weight;
    bucket->weightedVelocity.z += sample.relativeVelocity.z * weight;
    bucket->weight += weight;
    bucket->accumulatedImpulse += impulse;
    bucket->maximumMass = std::max(bucket->maximumMass, sample.effectiveMass);
    bucket->persistent = bucket->persistent || sample.persistent;
    bucket->latestTime = std::max(bucket->latestTime, sample.timeSeconds);
    return true;
}

PhysicsContactAudio PhysicsContactAudioAccumulator::emit(const Bucket& bucket) const noexcept {
    const float inverseWeight = bucket.weight > 0.0F ? 1.0F / bucket.weight : 0.0F;
    PhysicsContactAudio result;
    result.position = {bucket.weightedPosition.x * inverseWeight,
                       bucket.weightedPosition.y * inverseWeight,
                       bucket.weightedPosition.z * inverseWeight};
    result.relativeVelocity = {bucket.weightedVelocity.x * inverseWeight,
                               bucket.weightedVelocity.y * inverseWeight,
                               bucket.weightedVelocity.z * inverseWeight};
    result.materialA = bucket.materialA;
    result.materialB = bucket.materialB;
    result.normalImpulse = bucket.accumulatedImpulse;
    result.effectiveMass = bucket.maximumMass;
    result.persistent = bucket.persistent;
    return result;
}

std::size_t PhysicsContactAudioAccumulator::drain(double nowSeconds,
                                                   std::vector<PhysicsContactAudio>& output,
                                                   bool flushAll) {
    if (!std::isfinite(nowSeconds)) return 0;
    std::size_t emitted{};
    for (std::size_t index = 0; index < settings_.capacity; ++index) {
        Bucket& bucket = buckets_[index];
        if (!bucket.active) continue;
        const double windowEnd = (static_cast<double>(bucket.window) + 1.0) *
                                 settings_.aggregationWindowSeconds;
        if (!flushAll && nowSeconds < windowEnd) continue;
        output.push_back(emit(bucket));
        bucket = {};
        ++emitted;
        ++metrics_.emittedContacts;
        --metrics_.activeBuckets;
    }
    return emitted;
}

void PhysicsContactAudioAccumulator::reset() noexcept {
    for (auto& bucket : buckets_) bucket = {};
    metrics_ = {};
}

PhysicsContactAccumulatorMetrics PhysicsContactAudioAccumulator::metrics() const noexcept {
    return metrics_;
}

} // namespace dve::audio
