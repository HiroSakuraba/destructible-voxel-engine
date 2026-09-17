#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/replication_contract.hpp"

namespace dve {

using ReplicationPeerId = std::uint32_t;
inline constexpr ReplicationPeerId kInvalidReplicationPeerId = 0U;
using SimulatedPeerId = ReplicationPeerId;
inline constexpr SimulatedPeerId kInvalidSimulatedPeerId = kInvalidReplicationPeerId;

enum class ReplicationTransportChannel : std::uint8_t {
    ReliableOrdered,
    UnreliableSequenced,
};

enum class ReplicationMessageKind : std::uint8_t {
    GameplaySnapshot,
    InputCommand,
    DestructionBatch,
    SpawnDespawn,
    CheckpointFragment,
    BrickRepairRequest,
    BrickRepairResponse,
    UserPayload,
};

struct SimulatedNetworkConfig {
    std::uint32_t minimumLatencyTicks{2U};
    std::uint32_t maximumLatencyTicks{6U};
    std::uint32_t reorderExtraTicks{4U};
    std::uint16_t lossPermille{100U};
    std::uint16_t duplicationPermille{30U};
    std::uint16_t reorderPermille{150U};
    std::size_t maximumDatagramBytes{1200U};
    std::size_t bandwidthBytesPerTick{64U * 1024U};
    std::size_t maximumMessageBytes{4U * 1024U * 1024U};
    std::size_t maximumInFlightReliableMessages{4096U};
    std::size_t maximumReassemblies{4096U};
    std::uint32_t reliableRetryTicks{8U};
    std::uint32_t maximumReliableAttempts{128U};
    std::uint32_t reassemblyTimeoutTicks{240U};
    std::uint64_t seed{0xD6E146A5ULL};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct ReplicationTransportMessage {
    SimulatedPeerId source{kInvalidSimulatedPeerId};
    SimulatedPeerId destination{kInvalidSimulatedPeerId};
    ReplicationTransportChannel channel{ReplicationTransportChannel::ReliableOrdered};
    ReplicationMessageKind kind{ReplicationMessageKind::UserPayload};
    std::uint64_t sequence{};
    std::uint64_t deliveryTick{};
    std::vector<std::uint8_t> payload;
};

struct ReplicationTransportTelemetry {
    std::uint64_t logicalMessagesQueued{};
    std::uint64_t logicalMessagesDelivered{};
    std::uint64_t reliableMessagesAcknowledged{};
    std::uint64_t reliableRetransmissions{};
    std::uint64_t reliableFailures{};
    std::uint64_t datagramsAttempted{};
    std::uint64_t datagramsScheduled{};
    std::uint64_t datagramsDelivered{};
    std::uint64_t datagramsDropped{};
    std::uint64_t datagramsDuplicated{};
    std::uint64_t staleUnreliableDatagrams{};
    std::uint64_t duplicateReliableMessages{};
    std::uint64_t reassembliesExpired{};
    std::uint64_t bytesAttempted{};
    std::uint64_t bytesDelivered{};
    std::uint64_t peakPendingReliable{};
    std::uint64_t peakReassemblies{};
    std::uint64_t handshakesStarted{};
    std::uint64_t handshakesCompleted{};
    std::uint64_t handshakesFailed{};
    std::uint64_t authenticationFailures{};
    std::uint64_t datagramsRejected{};
};

struct ReplicationTransportCapabilities {
    std::size_t maximumMessageBytes{};
    std::size_t preferredDatagramBytes{};
    bool reliableOrdered{true};
    bool unreliableSequenced{true};
    bool simulated{};
    bool authenticated{};
    bool encrypted{};
};

// Socket-neutral contract used by gameplay replication. Backends may wrap UDP/QUIC, ENet,
// Steam Networking Sockets, console transports, or the deterministic simulator below.
class IReplicationTransport {
public:
    virtual ~IReplicationTransport() = default;
    [[nodiscard]] virtual std::optional<std::uint64_t> send(
        ReplicationPeerId source, ReplicationPeerId destination,
        ReplicationTransportChannel channel, ReplicationMessageKind kind,
        std::span<const std::uint8_t> payload, std::string* error = nullptr) = 0;
    virtual void update() = 0;
    [[nodiscard]] virtual std::vector<ReplicationTransportMessage> receive(
        ReplicationPeerId peer) = 0;
    [[nodiscard]] virtual std::uint64_t tick() const noexcept = 0;
    [[nodiscard]] virtual bool idle() const noexcept = 0;
    [[nodiscard]] virtual const ReplicationTransportTelemetry& telemetry() const noexcept = 0;
    [[nodiscard]] virtual ReplicationTransportCapabilities capabilities() const noexcept = 0;
};

class ReplicationTransportEndpoint {
public:
    ReplicationTransportEndpoint(IReplicationTransport& transport, ReplicationPeerId localPeer)
        : transport_(&transport), localPeer_(localPeer) {}
    [[nodiscard]] ReplicationPeerId local_peer() const noexcept { return localPeer_; }
    [[nodiscard]] std::optional<std::uint64_t> send(
        ReplicationPeerId destination, ReplicationTransportChannel channel,
        ReplicationMessageKind kind, std::span<const std::uint8_t> payload,
        std::string* error = nullptr);
    void update() { transport_->update(); }
    [[nodiscard]] std::vector<ReplicationTransportMessage> receive();
    [[nodiscard]] const ReplicationTransportTelemetry& telemetry() const noexcept {
        return transport_->telemetry();
    }
    [[nodiscard]] ReplicationTransportCapabilities capabilities() const noexcept {
        return transport_->capabilities();
    }
private:
    IReplicationTransport* transport_{};
    ReplicationPeerId localPeer_{kInvalidReplicationPeerId};
};

// Deterministic tick-based network and reliability harness. It intentionally models datagrams,
// fragmentation, ACK loss, retransmission, duplication, reordering, bandwidth pressure, reliable
// ordered delivery, and unreliable sequenced delivery without claiming to be a production socket
// transport.
class DeterministicReplicationTransport final : public IReplicationTransport {
public:
    explicit DeterministicReplicationTransport(SimulatedNetworkConfig config = {});
    ~DeterministicReplicationTransport();
    DeterministicReplicationTransport(const DeterministicReplicationTransport&) = delete;
    DeterministicReplicationTransport& operator=(const DeterministicReplicationTransport&) = delete;
    DeterministicReplicationTransport(DeterministicReplicationTransport&&) noexcept;
    DeterministicReplicationTransport& operator=(DeterministicReplicationTransport&&) noexcept;

    [[nodiscard]] std::optional<std::uint64_t> send(
        ReplicationPeerId source, ReplicationPeerId destination,
        ReplicationTransportChannel channel, ReplicationMessageKind kind,
        std::span<const std::uint8_t> payload, std::string* error = nullptr) override;
    [[nodiscard]] std::optional<std::uint64_t> send_reliable(
        SimulatedPeerId source, SimulatedPeerId destination, ReplicationMessageKind kind,
        std::span<const std::uint8_t> payload, std::string* error = nullptr);
    [[nodiscard]] std::optional<std::uint64_t> send_unreliable(
        SimulatedPeerId source, SimulatedPeerId destination, ReplicationMessageKind kind,
        std::span<const std::uint8_t> payload, std::string* error = nullptr);

    void advance_tick();
    void update() override { advance_tick(); }
    [[nodiscard]] std::vector<ReplicationTransportMessage> receive(SimulatedPeerId peer) override;
    [[nodiscard]] std::uint64_t tick() const noexcept override;
    [[nodiscard]] bool idle() const noexcept override;
    [[nodiscard]] std::size_t pending_reliable_messages() const noexcept;
    [[nodiscard]] const ReplicationTransportTelemetry& telemetry() const noexcept override;
    [[nodiscard]] const SimulatedNetworkConfig& config() const noexcept;
    [[nodiscard]] ReplicationTransportCapabilities capabilities() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ReplicatedInputCommand {
    NetworkObjectId pawn{kInvalidNetworkObjectId};
    std::uint64_t clientTick{};
    std::int16_t moveX{};
    std::int16_t moveY{};
    std::uint8_t flags{}; // bit 0 jump pressed, bit 1 crouch held

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] ReplicatedInputCommand make_replicated_input_command(
    NetworkObjectId pawn, std::uint64_t clientTick, const CharacterInput& input) noexcept;
[[nodiscard]] CharacterInput decode_replicated_input(const ReplicatedInputCommand& command) noexcept;
[[nodiscard]] std::vector<std::uint8_t> encode_replicated_input_command(
    const ReplicatedInputCommand& command, std::string* error = nullptr);
[[nodiscard]] std::optional<ReplicatedInputCommand> decode_replicated_input_command(
    std::span<const std::uint8_t> bytes, std::string* error = nullptr);

struct ReplicatedBrickRepairRequest {
    NetworkObjectId object{kInvalidNetworkObjectId};
    Int3 brick{};
    std::uint64_t causingEditSequence{};
    std::uint32_t expectedRevision{};
    std::uint32_t observedRevision{};
    std::uint64_t expectedContentHash{};
    std::uint64_t observedContentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] std::vector<std::uint8_t> encode_brick_repair_request(
    const ReplicatedBrickRepairRequest& request, std::string* error = nullptr);
[[nodiscard]] std::optional<ReplicatedBrickRepairRequest> decode_brick_repair_request(
    std::span<const std::uint8_t> bytes, std::string* error = nullptr);

struct ReplicatedBrickRepairResponse {
    NetworkObjectId object{kInvalidNetworkObjectId};
    Int3 brick{};
    std::uint64_t causingEditSequence{};
    std::uint32_t authoritativeRevision{};
    std::uint64_t authoritativeContentHash{};
    std::array<MaterialId, kBrickVoxelCount> materials{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] std::vector<std::uint8_t> encode_brick_repair_response(
    const ReplicatedBrickRepairResponse& response, std::string* error = nullptr);
[[nodiscard]] std::optional<ReplicatedBrickRepairResponse> decode_brick_repair_response(
    std::span<const std::uint8_t> bytes, std::string* error = nullptr);
[[nodiscard]] std::optional<ReplicatedBrickRepairResponse> build_brick_repair_response(
    const GameWorld& authority, const ReplicationIdentityRegistry& identities,
    const ReplicatedBrickRepairRequest& request, std::string* error = nullptr);
[[nodiscard]] bool apply_brick_repair_response(
    GameWorld& replica, const ReplicationIdentityRegistry& identities,
    const ReplicatedBrickRepairResponse& response, std::string* error = nullptr);

enum class DestructionReplicaResult : std::uint8_t {
    Applied,
    Duplicate,
    SequenceGap,
    RepairRequired,
    Invalid,
};

struct ClientPredictionConfig {
    float positionToleranceMeters{0.05F};
    float velocityToleranceMetersPerSecond{0.20F};
    float hardSnapDistanceMeters{1.0F};
    std::size_t maximumHistory{256U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct ClientPredictionRecord {
    ReplicatedInputCommand input{};
    ReplicatedCharacterState predicted{};
};

struct ClientReconciliationResult {
    std::uint64_t authoritativeTick{};
    Float3 authoritativePosition{};
    Float3 authoritativeVelocity{};
    float positionErrorMeters{};
    float velocityErrorMetersPerSecond{};
    bool correctionRequired{};
    bool hardSnap{};
    bool matchingPredictionFound{};
    std::vector<ReplicatedInputCommand> inputsToReplay;
};

// Bounded client-side prediction history. The engine-specific caller applies the authoritative
// state and re-simulates inputsToReplay through GameplayRuntime, keeping transport policy separate
// from character collision and input execution.
class ClientPredictionBuffer {
public:
    explicit ClientPredictionBuffer(ClientPredictionConfig config = {});
    [[nodiscard]] bool push(
        const ReplicatedInputCommand& input, const ReplicatedCharacterState& predicted,
        std::string* error = nullptr);
    [[nodiscard]] std::optional<ClientReconciliationResult> reconcile(
        std::uint64_t authoritativeTick, const ReplicatedCharacterState& authoritative,
        std::string* error = nullptr);
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    void clear() noexcept { records_.clear(); }
    [[nodiscard]] const ClientPredictionConfig& config() const noexcept { return config_; }

private:
    ClientPredictionConfig config_{};
    std::vector<ClientPredictionRecord> records_;
};

struct ClientPredictionExecutionResult {
    ClientReconciliationResult reconciliation{};
    std::size_t replayedInputs{};
    Float3 finalPosition{};
    Float3 finalVelocity{};
    bool authoritativeStateApplied{};
};

class ClientGameplayPrediction {
public:
    ClientGameplayPrediction(
        GameWorld& world, ReplicationIdentityRegistry& identities,
        GameObjectId localPawn, float fixedDeltaSeconds,
        ClientPredictionConfig config = {});
    [[nodiscard]] bool predict(
        std::uint64_t clientTick, const CharacterInput& input,
        std::string* error = nullptr);
    [[nodiscard]] std::optional<ClientPredictionExecutionResult> reconcile(
        std::uint64_t authoritativeTick, const ReplicatedCharacterState& authoritative,
        std::string* error = nullptr);
    [[nodiscard]] std::size_t history_size() const noexcept { return predictions_.size(); }
    [[nodiscard]] NetworkObjectId network_pawn() const noexcept { return networkPawn_; }
private:
    [[nodiscard]] std::optional<ReplicatedCharacterState> capture(std::string* error) const;
    GameWorld* world_{};
    ReplicationIdentityRegistry* identities_{};
    GameObjectId localPawn_{kInvalidGameObjectId};
    NetworkObjectId networkPawn_{kInvalidNetworkObjectId};
    float fixedDeltaSeconds_{1.0F / 60.0F};
    ClientPredictionBuffer predictions_{};
};

class DestructionReplicaTracker {
public:
    [[nodiscard]] DestructionReplicaResult accept(
        const ReplicatedDestructionEdit& edit, std::uint32_t observedRevision,
        std::uint64_t observedContentHash,
        ReplicatedBrickRepairRequest* repairRequest = nullptr,
        std::string* error = nullptr) noexcept;
    [[nodiscard]] std::uint64_t last_applied_sequence() const noexcept { return lastAppliedSequence_; }
    void reset(std::uint64_t lastAppliedSequence = 0U) noexcept {
        lastAppliedSequence_ = lastAppliedSequence;
    }

private:
    std::uint64_t lastAppliedSequence_{};
};

} // namespace dve
