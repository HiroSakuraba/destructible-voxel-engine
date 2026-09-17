#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "dve/network_crypto.hpp"
#include "dve/network_simulation.hpp"

namespace dve {

enum class ServerInputRejection : std::uint8_t {
    NoRejection,
    UnknownPeer,
    PawnNotOwned,
    InvalidCommand,
    TickTooOld,
    TickTooFarAhead,
    NonMonotonicTick,
    MovementOutOfRange,
    RateLimited,
    JumpRateLimited,
};

struct ServerInputValidationConfig {
    std::uint64_t maximumPastTicks{180U};
    std::uint64_t maximumFutureTicks{8U};
    std::uint32_t maximumCommandsPerServerTick{4U};
    std::uint64_t minimumJumpIntervalTicks{3U};
    float maximumMoveMagnitude{1.01F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct ServerInputValidationResult {
    bool accepted{};
    ServerInputRejection rejection{ServerInputRejection::NoRejection};
    CharacterInput input{};
    std::uint64_t clientTick{};
};

class ServerInputValidator {
public:
    explicit ServerInputValidator(ServerInputValidationConfig config = {});
    [[nodiscard]] bool bind_owned_pawn(
        ReplicationPeerId peer, NetworkObjectId pawn, std::string* error = nullptr);
    bool unbind_peer(ReplicationPeerId peer);
    [[nodiscard]] std::optional<NetworkObjectId> owned_pawn(ReplicationPeerId peer) const noexcept;
    [[nodiscard]] ServerInputValidationResult accept(
        ReplicationPeerId peer, std::uint64_t serverTick,
        const ReplicatedInputCommand& command) noexcept;
    void reset() noexcept;

private:
    struct PeerState {
        NetworkObjectId pawn{kInvalidNetworkObjectId};
        std::uint64_t lastClientTick{};
        std::uint64_t lastJumpTick{};
        std::uint64_t commandServerTick{};
        std::uint32_t commandsThisServerTick{};
    };
    ServerInputValidationConfig config_{};
    std::unordered_map<ReplicationPeerId, PeerState> peers_;
};

struct ServerInputExecutionResult {
    ServerInputValidationResult validation{};
    GameObjectId localPawn{kInvalidGameObjectId};
    bool applied{};
};

[[nodiscard]] ServerInputExecutionResult validate_and_apply_server_input(
    GameWorld& world, const ReplicationIdentityRegistry& identities,
    ServerInputValidator& validator, ReplicationPeerId peer, std::uint64_t serverTick,
    const ReplicatedInputCommand& command, std::string* error = nullptr);

struct RemoteInterpolationConfig {
    double interpolationDelayTicks{6.0};
    double maximumExtrapolationTicks{3.0};
    double secondsPerTick{1.0 / 60.0};
    float teleportDistanceMeters{4.0F};
    std::size_t maximumSnapshots{64U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct RemoteCharacterSample {
    NetworkObjectId pawn{kInvalidNetworkObjectId};
    double renderTick{};
    Float3 position{};
    Float3 velocity{};
    bool grounded{};
    bool crouched{};
    NetworkObjectId support{kInvalidNetworkObjectId};
    bool extrapolated{};
    bool teleportSnap{};
};

class RemoteCharacterInterpolator {
public:
    explicit RemoteCharacterInterpolator(RemoteInterpolationConfig config = {});
    [[nodiscard]] bool push(
        std::uint64_t serverTick, const ReplicatedCharacterState& state,
        std::string* error = nullptr);
    [[nodiscard]] std::optional<RemoteCharacterSample> sample(
        double estimatedServerTick) const noexcept;
    [[nodiscard]] std::size_t snapshot_count() const noexcept { return snapshots_.size(); }
    void clear() noexcept { snapshots_.clear(); }

private:
    struct Snapshot {
        std::uint64_t serverTick{};
        ReplicatedCharacterState state{};
    };
    RemoteInterpolationConfig config_{};
    std::deque<Snapshot> snapshots_;
};

[[nodiscard]] bool apply_remote_character_sample(
    GameWorld& world, const ReplicationIdentityRegistry& identities,
    const RemoteCharacterSample& sample, std::string* error = nullptr);

enum class LateJoinCheckpointPacketType : std::uint8_t { Begin = 1, Chunk = 2, Commit = 3 };

struct LateJoinCheckpointManifest {
    std::uint64_t checkpointId{};
    std::uint64_t serverTick{};
    std::uint64_t baselineEditSequence{};
    std::uint64_t totalBytes{};
    std::uint32_t chunkBytes{};
    std::uint32_t chunkCount{};
    NetworkSha256Digest contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct LateJoinCheckpointPacket {
    LateJoinCheckpointPacketType type{LateJoinCheckpointPacketType::Begin};
    LateJoinCheckpointManifest manifest{};
    std::uint32_t chunkIndex{};
    std::vector<std::uint8_t> chunk;
};

[[nodiscard]] std::vector<std::uint8_t> encode_late_join_checkpoint_packet(
    const LateJoinCheckpointPacket& packet, std::string* error = nullptr);
[[nodiscard]] std::optional<LateJoinCheckpointPacket> decode_late_join_checkpoint_packet(
    std::span<const std::uint8_t> bytes, std::string* error = nullptr);

class LateJoinCheckpointSender {
public:
    [[nodiscard]] bool begin(
        std::uint64_t checkpointId, std::uint64_t serverTick,
        std::uint64_t baselineEditSequence, std::span<const std::uint8_t> checkpoint,
        std::uint32_t chunkBytes = 32U * 1024U, std::string* error = nullptr);
    [[nodiscard]] std::size_t pump(
        ReplicationTransportEndpoint& endpoint, ReplicationPeerId destination,
        std::size_t maximumChunks, std::string* error = nullptr);
    [[nodiscard]] bool enqueued() const noexcept { return commitSent_; }
    [[nodiscard]] const LateJoinCheckpointManifest& manifest() const noexcept { return manifest_; }
    void reset() noexcept;

private:
    LateJoinCheckpointManifest manifest_{};
    std::vector<std::uint8_t> checkpoint_;
    std::uint32_t nextChunk_{};
    bool beginSent_{};
    bool commitSent_{};
};

struct LateJoinReceiverConfig {
    std::uint64_t maximumCheckpointBytes{64U * 1024U * 1024U};
    std::uint32_t maximumChunks{8192U};
    std::size_t maximumBufferedDeltaMessages{4096U};
    std::size_t maximumBufferedDeltaBytes{16U * 1024U * 1024U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

class LateJoinCheckpointReceiver {
public:
    explicit LateJoinCheckpointReceiver(LateJoinReceiverConfig config = {});
    [[nodiscard]] bool accept(
        const ReplicationTransportMessage& message, std::string* error = nullptr);
    [[nodiscard]] bool buffer_delta(
        const ReplicationTransportMessage& message, std::string* error = nullptr);
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] const LateJoinCheckpointManifest* manifest() const noexcept;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> take_checkpoint();
    [[nodiscard]] std::vector<ReplicationTransportMessage> take_buffered_deltas();
    void reset() noexcept;

private:
    LateJoinReceiverConfig config_{};
    std::optional<LateJoinCheckpointManifest> manifest_;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<bool> received_;
    std::size_t receivedBytes_{};
    bool commitReceived_{};
    bool complete_{};
    std::vector<std::uint8_t> assembled_;
    std::vector<ReplicationTransportMessage> bufferedDeltas_;
    std::size_t bufferedDeltaBytes_{};
};

} // namespace dve
