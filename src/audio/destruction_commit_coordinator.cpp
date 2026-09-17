#include "dve/audio/destruction_commit_coordinator.hpp"

#include <cmath>
#include <utility>

namespace dve::audio {

DestructionCommitCoordinator::DestructionCommitCoordinator(DestructionCommitLimits limits) noexcept
    : limits_(limits) {}

bool DestructionCommitCoordinator::begin(std::uint64_t generation, double timeSeconds,
                                         std::string* error) noexcept {
    if (active_) {
        if (error) *error = "a destruction transaction is already active";
        return false;
    }
    if (generation == 0U || generation <= metrics_.lastCommittedGeneration ||
        !std::isfinite(timeSeconds)) {
        if (error) *error = "destruction transaction generation/time is invalid or stale";
        return false;
    }
    commit_ = {};
    commit_.generation = generation;
    commit_.timeSeconds = timeSeconds;
    active_ = true;
    ++metrics_.begunTransactions;
    return true;
}

bool DestructionCommitCoordinator::record(VoxelAudioEdit value) noexcept {
    return append(commit_.voxelEdits, std::move(value), limits_.voxelEdits);
}
bool DestructionCommitCoordinator::record(PhysicsContactAudio value) noexcept {
    return append(commit_.contacts, std::move(value), limits_.contacts);
}
bool DestructionCommitCoordinator::record(FragmentSplitAudio value) noexcept {
    return append(commit_.fragmentSplits, std::move(value), limits_.fragmentSplits);
}
bool DestructionCommitCoordinator::record(StructuralStrainAudio value) noexcept {
    return append(commit_.structuralStrain, std::move(value), limits_.structuralStrain);
}

std::size_t DestructionCommitCoordinator::drain_contacts(
    PhysicsContactAudioAccumulator& accumulator, double nowSeconds, bool flushAll) {
    if (!active_) return 0;
    const std::size_t before = commit_.contacts.size();
    try {
        accumulator.drain(nowSeconds, commit_.contacts, flushAll);
    } catch (...) {
        ++metrics_.rejectedRecords;
    }
    if (commit_.contacts.size() > limits_.contacts) {
        metrics_.rejectedRecords += commit_.contacts.size() - limits_.contacts;
        commit_.contacts.resize(limits_.contacts);
    }
    return commit_.contacts.size() - before;
}

bool DestructionCommitCoordinator::set_acoustic_build(AcousticBuildRequest request) noexcept {
    if (!active_) {
        ++metrics_.rejectedRecords;
        return false;
    }
    try {
        request.sourceGeneration = commit_.generation;
        commit_.acousticBuild = std::move(request);
        return true;
    } catch (...) {
        ++metrics_.rejectedRecords;
        return false;
    }
}

bool DestructionCommitCoordinator::commit(DestructionAudioIngress& ingress,
                                           std::string* error) noexcept {
    if (!active_) {
        if (error) *error = "no destruction transaction is active";
        ++metrics_.rejectedCommits;
        return false;
    }
    if (commit_.acousticBuild) commit_.acousticBuild->sourceGeneration = commit_.generation;
    const std::uint64_t generation = commit_.generation;
    if (!ingress.try_submit(std::move(commit_))) {
        if (error) *error = "destruction ingress queue is full";
        ++metrics_.rejectedCommits;
        commit_ = {};
        active_ = false;
        return false;
    }
    metrics_.lastCommittedGeneration = generation;
    ++metrics_.committedTransactions;
    commit_ = {};
    active_ = false;
    return true;
}

void DestructionCommitCoordinator::cancel() noexcept {
    if (active_) ++metrics_.cancelledTransactions;
    commit_ = {};
    active_ = false;
}

DestructionCommitCoordinatorMetrics DestructionCommitCoordinator::metrics() const noexcept {
    return metrics_;
}

} // namespace dve::audio
