#include "dve/audio/destruction_ingress.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::audio {

bool DestructionAudioIngress::try_submit(DestructionAudioCommit commit) noexcept {
    if (commit.generation == 0U || !std::isfinite(commit.timeSeconds)) {
        rejectedCommits_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const std::size_t write = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t read = readIndex_.load(std::memory_order_acquire);
    if (write - read >= kDestructionAudioCommitQueueCapacity) {
        rejectedCommits_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    try {
        auto snapshot = std::make_shared<DestructionAudioCommit>(std::move(commit));
        queue_[write & mask_] = std::move(snapshot);
    } catch (...) {
        rejectedCommits_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const auto generation = queue_[write & mask_]->generation;
    writeIndex_.store(write + 1U, std::memory_order_release);
    submittedCommits_.fetch_add(1U, std::memory_order_relaxed);
    lastSubmittedGeneration_.store(generation, std::memory_order_relaxed);
    return true;
}

std::size_t DestructionAudioIngress::drain(DestructionAudioRuntime& runtime,
                                           AsyncAcousticPublisher& acoustics,
                                           double nowSeconds,
                                           bool flushAll,
                                           std::size_t maxCommits) noexcept {
    std::size_t consumed{};
    std::size_t read = readIndex_.load(std::memory_order_relaxed);
    const std::size_t write = writeIndex_.load(std::memory_order_acquire);
    std::uint64_t lastGeneration = lastDrainedGeneration_.load(std::memory_order_relaxed);
    while (read != write && consumed < maxCommits) {
        CommitPtr commit = std::move(queue_[read & mask_]);
        queue_[read & mask_].reset();
        ++read;
        ++consumed;
        if (!commit) continue;
        if (commit->generation <= lastGeneration) {
            staleCommits_.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        runtime.submit_voxel_edits(commit->voxelEdits, commit->timeSeconds);
        runtime.submit_contacts(commit->contacts, commit->timeSeconds);
        runtime.submit_fragment_splits(commit->fragmentSplits, commit->timeSeconds);
        runtime.submit_structural_strain(commit->structuralStrain, commit->timeSeconds);
        if (commit->acousticBuild) {
            commit->acousticBuild->sourceGeneration = commit->generation;
            (void)acoustics.submit(std::move(*commit->acousticBuild));
            acousticRequests_.fetch_add(1U, std::memory_order_relaxed);
        }
        lastGeneration = commit->generation;
        drainedCommits_.fetch_add(1U, std::memory_order_relaxed);
    }
    readIndex_.store(read, std::memory_order_release);
    lastDrainedGeneration_.store(lastGeneration, std::memory_order_relaxed);
    const std::size_t actions = runtime.update(nowSeconds, flushAll);
    dispatchedActions_.fetch_add(actions, std::memory_order_relaxed);
    return actions;
}

void DestructionAudioIngress::discard_pending() noexcept {
    std::size_t read = readIndex_.load(std::memory_order_relaxed);
    const std::size_t write = writeIndex_.load(std::memory_order_acquire);
    while (read != write) {
        queue_[read & mask_].reset();
        ++read;
    }
    readIndex_.store(read, std::memory_order_release);
}

DestructionAudioIngressMetrics DestructionAudioIngress::metrics() const noexcept {
    DestructionAudioIngressMetrics result;
    result.submittedCommits = submittedCommits_.load(std::memory_order_relaxed);
    result.rejectedCommits = rejectedCommits_.load(std::memory_order_relaxed);
    result.drainedCommits = drainedCommits_.load(std::memory_order_relaxed);
    result.staleCommits = staleCommits_.load(std::memory_order_relaxed);
    result.acousticRequests = acousticRequests_.load(std::memory_order_relaxed);
    result.dispatchedActions = dispatchedActions_.load(std::memory_order_relaxed);
    result.lastSubmittedGeneration = lastSubmittedGeneration_.load(std::memory_order_relaxed);
    result.lastDrainedGeneration = lastDrainedGeneration_.load(std::memory_order_relaxed);
    const std::size_t write = writeIndex_.load(std::memory_order_acquire);
    const std::size_t read = readIndex_.load(std::memory_order_acquire);
    result.approximateQueuedCommits = write - read;
    return result;
}

} // namespace dve::audio
