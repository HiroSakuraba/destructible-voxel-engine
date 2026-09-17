#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "dve/audio/destruction_ingress.hpp"
#include "dve/audio/physics_contact_audio_accumulator.hpp"

namespace dve::audio {

struct DestructionCommitLimits {
    std::size_t voxelEdits{65536};
    std::size_t contacts{4096};
    std::size_t fragmentSplits{4096};
    std::size_t structuralStrain{4096};
};

struct DestructionCommitCoordinatorMetrics {
    std::uint64_t begunTransactions{};
    std::uint64_t committedTransactions{};
    std::uint64_t cancelledTransactions{};
    std::uint64_t rejectedRecords{};
    std::uint64_t rejectedCommits{};
    std::uint64_t lastCommittedGeneration{};
};

// Authoritative transaction recorder. All audio stimuli and the acoustic dirty-region request are
// moved into one DestructionAudioCommit, and the source generation is rewritten at the single
// publication point. A failed queue submission leaves no partially published acoustic state.
class DestructionCommitCoordinator {
public:
    explicit DestructionCommitCoordinator(DestructionCommitLimits limits = {}) noexcept;

    bool begin(std::uint64_t generation, double timeSeconds,
               std::string* error = nullptr) noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return commit_.generation; }

    bool record(VoxelAudioEdit value) noexcept;
    bool record(PhysicsContactAudio value) noexcept;
    bool record(FragmentSplitAudio value) noexcept;
    bool record(StructuralStrainAudio value) noexcept;
    std::size_t drain_contacts(PhysicsContactAudioAccumulator& accumulator,
                               double nowSeconds, bool flushAll = false);
    bool set_acoustic_build(AcousticBuildRequest request) noexcept;

    [[nodiscard]] bool commit(DestructionAudioIngress& ingress,
                              std::string* error = nullptr) noexcept;
    void cancel() noexcept;
    [[nodiscard]] DestructionCommitCoordinatorMetrics metrics() const noexcept;

private:
    template <typename Value>
    bool append(std::vector<Value>& values, Value value, std::size_t limit) noexcept {
        if (!active_ || values.size() >= limit) {
            ++metrics_.rejectedRecords;
            return false;
        }
        try {
            values.push_back(std::move(value));
            return true;
        } catch (...) {
            ++metrics_.rejectedRecords;
            return false;
        }
    }

    DestructionCommitLimits limits_{};
    DestructionAudioCommit commit_{};
    bool active_{};
    DestructionCommitCoordinatorMetrics metrics_{};
};

} // namespace dve::audio
