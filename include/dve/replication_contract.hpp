#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "dve/gameplay_runtime.hpp"

namespace dve {

using NetworkObjectId = std::uint64_t;
inline constexpr NetworkObjectId kInvalidNetworkObjectId = 0;

struct QuantizedPosition {
    std::int32_t xMillimeters{};
    std::int32_t yMillimeters{};
    std::int32_t zMillimeters{};
};

struct QuantizedVelocity {
    std::int16_t xCentimetersPerSecond{};
    std::int16_t yCentimetersPerSecond{};
    std::int16_t zCentimetersPerSecond{};
};

struct ReplicatedCharacterState {
    NetworkObjectId pawn{kInvalidNetworkObjectId};
    QuantizedPosition position{};
    QuantizedVelocity velocity{};
    std::uint8_t flags{}; // bit 0 grounded, bit 1 crouched
    NetworkObjectId support{kInvalidNetworkObjectId};
};

struct ReplicatedTriggerState {
    NetworkObjectId trigger{kInvalidNetworkObjectId};
    bool enabled{};
    bool fired{};
};

struct ReplicatedDestructionEdit {
    NetworkObjectId object{kInvalidNetworkObjectId};
    std::uint64_t editSequence{};
    QuantizedPosition center{};
    std::uint32_t radiusMillimeters{};
    std::uint32_t expectedBrickRevision{};
    std::uint64_t expectedContentHash{};
    // Address of the brick whose revision/hash is being certified. v1 packets did not
    // carry this field and decode as brick {0,0,0}; v2 packets preserve it explicitly.
    Int3 brick{};
};

struct GameplayReplicationFrame {
    std::uint64_t serverTick{};
    std::uint64_t baselineTick{};
    std::vector<ReplicatedCharacterState> characters;
    std::vector<ReplicatedTriggerState> triggers;
    std::vector<ReplicatedDestructionEdit> destructionEdits;

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
    [[nodiscard]] std::uint64_t stable_hash() const noexcept;
};


class ReplicationIdentityRegistry {
public:
    [[nodiscard]] NetworkObjectId ensure_object(GameObjectId object);
    [[nodiscard]] NetworkObjectId ensure_trigger(GameTriggerId trigger);
    bool bind_object(GameObjectId object, NetworkObjectId networkId, std::string* error = nullptr);
    bool bind_trigger(GameTriggerId trigger, NetworkObjectId networkId, std::string* error = nullptr);
    bool release_object(GameObjectId object);
    bool release_trigger(GameTriggerId trigger);
    [[nodiscard]] std::optional<NetworkObjectId> network_object(GameObjectId object) const noexcept;
    [[nodiscard]] std::optional<NetworkObjectId> network_trigger(GameTriggerId trigger) const noexcept;
    [[nodiscard]] std::optional<GameObjectId> local_object(NetworkObjectId networkId) const noexcept;
    [[nodiscard]] std::optional<GameTriggerId> local_trigger(NetworkObjectId networkId) const noexcept;

private:
    [[nodiscard]] NetworkObjectId allocate();
    std::unordered_map<GameObjectId, NetworkObjectId> objects_;
    std::unordered_map<GameTriggerId, NetworkObjectId> triggers_;
    std::unordered_map<NetworkObjectId, GameObjectId> reverseObjects_;
    std::unordered_map<NetworkObjectId, GameTriggerId> reverseTriggers_;
    NetworkObjectId nextId_{1U};
};

class DestructionReplicationJournal {
public:
    explicit DestructionReplicationJournal(ReplicationIdentityRegistry& identities)
        : identities_(&identities) {}
    [[nodiscard]] std::uint64_t record(
        const GameDamageEvent& event, std::uint32_t brickRevision,
        std::uint64_t contentHash, Int3 brick = {});
    [[nodiscard]] std::vector<ReplicatedDestructionEdit> after(std::uint64_t sequence) const;
    void acknowledge_through(std::uint64_t sequence);
    [[nodiscard]] std::uint64_t latest_sequence() const noexcept { return nextSequence_ - 1U; }

private:
    ReplicationIdentityRegistry* identities_{};
    std::vector<ReplicatedDestructionEdit> edits_;
    std::uint64_t nextSequence_{1U};
};

[[nodiscard]] GameplayReplicationFrame build_gameplay_replication_frame(
    const GameWorld& world, ReplicationIdentityRegistry& identities,
    std::uint64_t serverTick, std::uint64_t baselineTick,
    std::span<const ReplicatedDestructionEdit> destructionEdits = {});

[[nodiscard]] QuantizedPosition quantize_position_millimeters(Float3 position) noexcept;
[[nodiscard]] Float3 dequantize_position_meters(QuantizedPosition position) noexcept;
[[nodiscard]] QuantizedVelocity quantize_velocity_centimeters(Float3 velocity) noexcept;
[[nodiscard]] Float3 dequantize_velocity_meters(QuantizedVelocity velocity) noexcept;

[[nodiscard]] std::vector<std::uint8_t> encode_gameplay_replication_frame(
    const GameplayReplicationFrame& frame, std::string* error = nullptr);
[[nodiscard]] std::optional<GameplayReplicationFrame> decode_gameplay_replication_frame(
    std::span<const std::uint8_t> bytes, std::string* error = nullptr);

} // namespace dve
