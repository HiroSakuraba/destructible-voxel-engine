#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "dve/audio/acoustic_runtime.hpp"
#include "dve/audio/destruction_runtime.hpp"

namespace dve::audio {

inline constexpr std::size_t kDestructionAudioCommitQueueCapacity = 32;

// One authoritative destruction commit. The voxel/physics integration thread constructs this
// owned snapshot after a world-generation commit, then publishes it without blocking. Audio and
// acoustic geometry therefore observe the same generation instead of independently racing.
struct DestructionAudioCommit {
    std::uint64_t generation{};
    double timeSeconds{};
    std::vector<VoxelAudioEdit> voxelEdits;
    std::vector<PhysicsContactAudio> contacts;
    std::vector<FragmentSplitAudio> fragmentSplits;
    std::vector<StructuralStrainAudio> structuralStrain;
    std::optional<AcousticBuildRequest> acousticBuild;
};

struct DestructionAudioIngressMetrics {
    std::uint64_t submittedCommits{};
    std::uint64_t rejectedCommits{};
    std::uint64_t drainedCommits{};
    std::uint64_t staleCommits{};
    std::uint64_t acousticRequests{};
    std::uint64_t dispatchedActions{};
    std::uint64_t lastSubmittedGeneration{};
    std::uint64_t lastDrainedGeneration{};
    std::size_t approximateQueuedCommits{};
};

// Bounded single-producer/single-consumer bridge. The producer is the authoritative simulation
// commit thread; the consumer is the audio control thread. Neither side waits for the other.
// Heavy vectors are owned by shared snapshots allocated before publication, while queue traffic
// itself is only pointer movement and atomics.
class DestructionAudioIngress {
public:
    DestructionAudioIngress() = default;
    DestructionAudioIngress(const DestructionAudioIngress&) = delete;
    DestructionAudioIngress& operator=(const DestructionAudioIngress&) = delete;

    [[nodiscard]] bool try_submit(DestructionAudioCommit commit) noexcept;

    // Drains at most maxCommits and then asks the destruction runtime to compile any clusters
    // that have matured. A generation older than the last drained generation is discarded.
    std::size_t drain(DestructionAudioRuntime& runtime,
                      AsyncAcousticPublisher& acoustics,
                      double nowSeconds,
                      bool flushAll = false,
                      std::size_t maxCommits = kDestructionAudioCommitQueueCapacity) noexcept;

    void discard_pending() noexcept;
    [[nodiscard]] DestructionAudioIngressMetrics metrics() const noexcept;

private:
    using CommitPtr = std::shared_ptr<DestructionAudioCommit>;
    static constexpr std::size_t mask_ = kDestructionAudioCommitQueueCapacity - 1U;
    static_assert((kDestructionAudioCommitQueueCapacity & mask_) == 0U,
                  "destruction commit queue capacity must be a power of two");

    std::array<CommitPtr, kDestructionAudioCommitQueueCapacity> queue_{};
    alignas(64) std::atomic<std::size_t> writeIndex_{};
    alignas(64) std::atomic<std::size_t> readIndex_{};

    std::atomic<std::uint64_t> submittedCommits_{};
    std::atomic<std::uint64_t> rejectedCommits_{};
    std::atomic<std::uint64_t> drainedCommits_{};
    std::atomic<std::uint64_t> staleCommits_{};
    std::atomic<std::uint64_t> acousticRequests_{};
    std::atomic<std::uint64_t> dispatchedActions_{};
    std::atomic<std::uint64_t> lastSubmittedGeneration_{};
    std::atomic<std::uint64_t> lastDrainedGeneration_{};
};

} // namespace dve::audio
