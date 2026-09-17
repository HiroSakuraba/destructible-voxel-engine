#include "dve/replication_contract.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace dve {
namespace {

constexpr std::uint32_t kMagic = 0x31525644U; // DVR1, little endian
constexpr std::uint16_t kCurrentVersion = 2U;
constexpr std::uint16_t kLegacyVersion = 1U;
constexpr std::size_t kMaximumRecordsPerKind = 65535U;

template <typename T>
void append_le(std::vector<std::uint8_t>& output, T value) {
    using U = std::make_unsigned_t<T>;
    U bits{};
    std::memcpy(&bits, &value, sizeof(value));
    const std::size_t offset = output.size();
    output.resize(offset + sizeof(T));
    for (std::size_t index = 0; index < sizeof(T); ++index)
        output[offset + index] = static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xFFU);
}

template <typename T>
bool read_le(std::span<const std::uint8_t> input, std::size_t& cursor, T& value) {
    if (cursor + sizeof(T) > input.size()) return false;
    using U = std::make_unsigned_t<T>;
    U bits{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        const std::uint64_t shifted = static_cast<std::uint64_t>(input[cursor + index]) << (index * 8U);
        bits = static_cast<U>(bits | static_cast<U>(shifted));
    }
    std::memcpy(&value, &bits, sizeof(value));
    cursor += sizeof(T);
    return true;
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value) noexcept { hash_bytes(hash, &value, sizeof(value)); }

std::int32_t quantize_mm(float value) noexcept {
    const double scaled = std::round(static_cast<double>(value) * 1000.0);
    return static_cast<std::int32_t>(std::clamp(
        scaled, static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

std::int16_t quantize_cms(float value) noexcept {
    const double scaled = std::round(static_cast<double>(value) * 100.0);
    return static_cast<std::int16_t>(std::clamp(
        scaled, static_cast<double>(std::numeric_limits<std::int16_t>::min()),
        static_cast<double>(std::numeric_limits<std::int16_t>::max())));
}

} // namespace


NetworkObjectId ReplicationIdentityRegistry::allocate() {
    const NetworkObjectId start = nextId_;
    do {
        if (nextId_ == kInvalidNetworkObjectId) ++nextId_;
        if (!reverseObjects_.contains(nextId_) && !reverseTriggers_.contains(nextId_)) {
            const NetworkObjectId allocated = nextId_;
            ++nextId_;
            return allocated;
        }
        ++nextId_;
    } while (nextId_ != start);
    return kInvalidNetworkObjectId;
}

NetworkObjectId ReplicationIdentityRegistry::ensure_object(GameObjectId object) {
    if (object == kInvalidGameObjectId) return kInvalidNetworkObjectId;
    if (const auto existing = network_object(object)) return *existing;
    const NetworkObjectId id = allocate();
    objects_[object] = id;
    reverseObjects_[id] = object;
    return id;
}

NetworkObjectId ReplicationIdentityRegistry::ensure_trigger(GameTriggerId trigger) {
    if (trigger == kInvalidGameTriggerId) return kInvalidNetworkObjectId;
    if (const auto existing = network_trigger(trigger)) return *existing;
    const NetworkObjectId id = allocate();
    triggers_[trigger] = id;
    reverseTriggers_[id] = trigger;
    return id;
}

bool ReplicationIdentityRegistry::bind_object(
    GameObjectId object, NetworkObjectId networkId, std::string* error) {
    if (object == kInvalidGameObjectId || networkId == kInvalidNetworkObjectId) {
        if (error) *error = "replication identity cannot bind an invalid id";
        return false;
    }
    if (const auto existing = network_object(object); existing) {
        if (*existing == networkId) return true;
        if (error) *error = "local object is already bound to a different network id";
        return false;
    }
    if (reverseObjects_.contains(networkId) || reverseTriggers_.contains(networkId)) {
        if (error) *error = "network id is already bound";
        return false;
    }
    objects_[object] = networkId;
    reverseObjects_[networkId] = object;
    if (networkId != std::numeric_limits<NetworkObjectId>::max())
        nextId_ = std::max(nextId_, networkId + 1U);
    return true;
}

bool ReplicationIdentityRegistry::bind_trigger(
    GameTriggerId trigger, NetworkObjectId networkId, std::string* error) {
    if (trigger == kInvalidGameTriggerId || networkId == kInvalidNetworkObjectId) {
        if (error) *error = "replication identity cannot bind an invalid id";
        return false;
    }
    if (const auto existing = network_trigger(trigger); existing) {
        if (*existing == networkId) return true;
        if (error) *error = "local trigger is already bound to a different network id";
        return false;
    }
    if (reverseObjects_.contains(networkId) || reverseTriggers_.contains(networkId)) {
        if (error) *error = "network id is already bound";
        return false;
    }
    triggers_[trigger] = networkId;
    reverseTriggers_[networkId] = trigger;
    if (networkId != std::numeric_limits<NetworkObjectId>::max())
        nextId_ = std::max(nextId_, networkId + 1U);
    return true;
}

bool ReplicationIdentityRegistry::release_object(GameObjectId object) {
    const auto it = objects_.find(object);
    if (it == objects_.end()) return false;
    reverseObjects_.erase(it->second);
    objects_.erase(it);
    return true;
}

bool ReplicationIdentityRegistry::release_trigger(GameTriggerId trigger) {
    const auto it = triggers_.find(trigger);
    if (it == triggers_.end()) return false;
    reverseTriggers_.erase(it->second);
    triggers_.erase(it);
    return true;
}

std::optional<NetworkObjectId> ReplicationIdentityRegistry::network_object(GameObjectId object) const noexcept {
    const auto it = objects_.find(object);
    return it == objects_.end() ? std::nullopt : std::optional<NetworkObjectId>{it->second};
}

std::optional<NetworkObjectId> ReplicationIdentityRegistry::network_trigger(GameTriggerId trigger) const noexcept {
    const auto it = triggers_.find(trigger);
    return it == triggers_.end() ? std::nullopt : std::optional<NetworkObjectId>{it->second};
}

std::optional<GameObjectId> ReplicationIdentityRegistry::local_object(NetworkObjectId networkId) const noexcept {
    const auto it = reverseObjects_.find(networkId);
    return it == reverseObjects_.end() ? std::nullopt : std::optional<GameObjectId>{it->second};
}

std::optional<GameTriggerId> ReplicationIdentityRegistry::local_trigger(NetworkObjectId networkId) const noexcept {
    const auto it = reverseTriggers_.find(networkId);
    return it == reverseTriggers_.end() ? std::nullopt : std::optional<GameTriggerId>{it->second};
}

std::uint64_t DestructionReplicationJournal::record(
    const GameDamageEvent& event, std::uint32_t brickRevision,
    std::uint64_t contentHash, Int3 brick) {
    const NetworkObjectId object = identities_->ensure_object(event.objectId);
    if (object == kInvalidNetworkObjectId || !std::isfinite(event.radius) || event.radius <= 0.0F)
        return 0U;
    const std::uint64_t sequence = nextSequence_++;
    edits_.push_back({
        object, sequence,
        quantize_position_millimeters(event.worldCenter),
        static_cast<std::uint32_t>(std::clamp(
            std::llround(static_cast<double>(event.radius) * 1000.0), 1LL,
            static_cast<long long>(std::numeric_limits<std::uint32_t>::max()))),
        brickRevision, contentHash, brick});
    return sequence;
}

std::vector<ReplicatedDestructionEdit> DestructionReplicationJournal::after(std::uint64_t sequence) const {
    std::vector<ReplicatedDestructionEdit> result;
    for (const auto& edit : edits_) if (edit.editSequence > sequence) result.push_back(edit);
    return result;
}

void DestructionReplicationJournal::acknowledge_through(std::uint64_t sequence) {
    std::erase_if(edits_, [sequence](const ReplicatedDestructionEdit& edit) {
        return edit.editSequence <= sequence;
    });
}

GameplayReplicationFrame build_gameplay_replication_frame(
    const GameWorld& world, ReplicationIdentityRegistry& identities,
    std::uint64_t serverTick, std::uint64_t baselineTick,
    std::span<const ReplicatedDestructionEdit> destructionEdits) {
    GameplayReplicationFrame frame;
    frame.serverTick = serverTick;
    frame.baselineTick = baselineTick;
    for (const GameObjectId pawn : world.gameplay().character_ids()) {
        const auto* state = world.gameplay().character(pawn);
        const auto position = world.position(pawn);
        if (!state || !position) continue;
        ReplicatedCharacterState replicated;
        replicated.pawn = identities.ensure_object(pawn);
        replicated.position = quantize_position_millimeters(*position);
        replicated.velocity = quantize_velocity_centimeters(state->velocity);
        replicated.flags = static_cast<std::uint8_t>((state->grounded ? 0x01U : 0U) |
            (state->stance == CharacterStance::Crouched ? 0x02U : 0U));
        if (state->supportObject != kInvalidGameObjectId)
            replicated.support = identities.ensure_object(state->supportObject);
        frame.characters.push_back(replicated);
    }
    for (const GameTriggerId triggerId : world.gameplay().trigger_ids()) {
        const TriggerVolumeDesc* trigger = world.gameplay().trigger(triggerId);
        if (!trigger) continue;
        frame.triggers.push_back({identities.ensure_trigger(triggerId), trigger->enabled,
                                  world.gameplay().trigger_fired(triggerId)});
    }
    frame.destructionEdits.assign(destructionEdits.begin(), destructionEdits.end());
    return frame;
}

QuantizedPosition quantize_position_millimeters(Float3 position) noexcept {
    return {quantize_mm(position.x), quantize_mm(position.y), quantize_mm(position.z)};
}

Float3 dequantize_position_meters(QuantizedPosition position) noexcept {
    return {static_cast<float>(position.xMillimeters) * 0.001F,
            static_cast<float>(position.yMillimeters) * 0.001F,
            static_cast<float>(position.zMillimeters) * 0.001F};
}

QuantizedVelocity quantize_velocity_centimeters(Float3 velocity) noexcept {
    return {quantize_cms(velocity.x), quantize_cms(velocity.y), quantize_cms(velocity.z)};
}

Float3 dequantize_velocity_meters(QuantizedVelocity velocity) noexcept {
    return {static_cast<float>(velocity.xCentimetersPerSecond) * 0.01F,
            static_cast<float>(velocity.yCentimetersPerSecond) * 0.01F,
            static_cast<float>(velocity.zCentimetersPerSecond) * 0.01F};
}

bool GameplayReplicationFrame::validate(std::string* error) const noexcept {
    const auto reject = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (baselineTick > serverTick) return reject("replication baseline cannot be newer than server tick");
    if (characters.size() > kMaximumRecordsPerKind || triggers.size() > kMaximumRecordsPerKind ||
        destructionEdits.size() > kMaximumRecordsPerKind) return reject("replication frame exceeds record limits");
    for (const auto& character : characters) {
        if (character.pawn == kInvalidNetworkObjectId) return reject("replicated character has an invalid pawn id");
        if ((character.flags & ~0x03U) != 0U) return reject("replicated character contains unknown flags");
    }
    for (const auto& trigger : triggers)
        if (trigger.trigger == kInvalidNetworkObjectId) return reject("replicated trigger has an invalid id");
    std::uint64_t previousSequence{};
    bool first = true;
    for (const auto& edit : destructionEdits) {
        if (edit.object == kInvalidNetworkObjectId || edit.radiusMillimeters == 0U)
            return reject("replicated destruction edit is invalid");
        if (!first && edit.editSequence <= previousSequence)
            return reject("destruction edits must be strictly ordered by sequence");
        first = false;
        previousSequence = edit.editSequence;
    }
    return true;
}

std::uint64_t GameplayReplicationFrame::stable_hash() const noexcept {
    const auto encoded = encode_gameplay_replication_frame(*this);
    std::uint64_t hash = 1469598103934665603ULL;
    if (!encoded.empty()) hash_bytes(hash, encoded.data(), encoded.size());
    return hash;
}

std::vector<std::uint8_t> encode_gameplay_replication_frame(
    const GameplayReplicationFrame& frame, std::string* error) {
    if (!frame.validate(error)) return {};
    std::vector<std::uint8_t> output;
    output.reserve(64U + frame.characters.size() * 43U + frame.triggers.size() * 10U +
                   frame.destructionEdits.size() * 60U);
    append_le(output, kMagic);
    append_le(output, kCurrentVersion);
    append_le(output, static_cast<std::uint16_t>(0U));
    append_le(output, frame.serverTick);
    append_le(output, frame.baselineTick);
    append_le(output, static_cast<std::uint32_t>(frame.characters.size()));
    append_le(output, static_cast<std::uint32_t>(frame.triggers.size()));
    append_le(output, static_cast<std::uint32_t>(frame.destructionEdits.size()));
    for (const auto& character : frame.characters) {
        append_le(output, character.pawn);
        append_le(output, character.position.xMillimeters);
        append_le(output, character.position.yMillimeters);
        append_le(output, character.position.zMillimeters);
        append_le(output, character.velocity.xCentimetersPerSecond);
        append_le(output, character.velocity.yCentimetersPerSecond);
        append_le(output, character.velocity.zCentimetersPerSecond);
        output.push_back(character.flags);
        output.push_back(0U);
        append_le(output, character.support);
    }
    for (const auto& trigger : frame.triggers) {
        append_le(output, trigger.trigger);
        output.push_back(trigger.enabled ? 1U : 0U);
        output.push_back(trigger.fired ? 1U : 0U);
    }
    for (const auto& edit : frame.destructionEdits) {
        append_le(output, edit.object);
        append_le(output, edit.editSequence);
        append_le(output, edit.center.xMillimeters);
        append_le(output, edit.center.yMillimeters);
        append_le(output, edit.center.zMillimeters);
        append_le(output, edit.radiusMillimeters);
        append_le(output, edit.expectedBrickRevision);
        append_le(output, edit.expectedContentHash);
        append_le(output, edit.brick.x);
        append_le(output, edit.brick.y);
        append_le(output, edit.brick.z);
    }
    return output;
}

std::optional<GameplayReplicationFrame> decode_gameplay_replication_frame(
    std::span<const std::uint8_t> bytes, std::string* error) {
    const auto reject = [&](const char* message) -> std::optional<GameplayReplicationFrame> {
        if (error) *error = message;
        return std::nullopt;
    };
    std::size_t cursor{};
    std::uint32_t magic{};
    std::uint16_t version{}, reserved{};
    GameplayReplicationFrame frame;
    std::uint32_t characterCount{}, triggerCount{}, destructionCount{};
    if (!read_le(bytes, cursor, magic) || !read_le(bytes, cursor, version) || !read_le(bytes, cursor, reserved) ||
        !read_le(bytes, cursor, frame.serverTick) || !read_le(bytes, cursor, frame.baselineTick) ||
        !read_le(bytes, cursor, characterCount) || !read_le(bytes, cursor, triggerCount) ||
        !read_le(bytes, cursor, destructionCount)) return reject("replication frame is truncated");
    if (magic != kMagic || (version != kLegacyVersion && version != kCurrentVersion) || reserved != 0U)
        return reject("replication frame header is unsupported");
    if (characterCount > kMaximumRecordsPerKind || triggerCount > kMaximumRecordsPerKind ||
        destructionCount > kMaximumRecordsPerKind) return reject("replication frame count is out of bounds");
    frame.characters.resize(characterCount);
    frame.triggers.resize(triggerCount);
    frame.destructionEdits.resize(destructionCount);
    for (auto& character : frame.characters) {
        std::uint8_t reservedByte{};
        if (!read_le(bytes, cursor, character.pawn) ||
            !read_le(bytes, cursor, character.position.xMillimeters) ||
            !read_le(bytes, cursor, character.position.yMillimeters) ||
            !read_le(bytes, cursor, character.position.zMillimeters) ||
            !read_le(bytes, cursor, character.velocity.xCentimetersPerSecond) ||
            !read_le(bytes, cursor, character.velocity.yCentimetersPerSecond) ||
            !read_le(bytes, cursor, character.velocity.zCentimetersPerSecond) ||
            cursor + 2U > bytes.size()) return reject("replicated character record is truncated");
        character.flags = bytes[cursor++];
        reservedByte = bytes[cursor++];
        if (reservedByte != 0U || !read_le(bytes, cursor, character.support))
            return reject("replicated character record is malformed");
    }
    for (auto& trigger : frame.triggers) {
        if (!read_le(bytes, cursor, trigger.trigger) || cursor + 2U > bytes.size())
            return reject("replicated trigger record is truncated");
        const std::uint8_t enabled = bytes[cursor++];
        const std::uint8_t fired = bytes[cursor++];
        if (enabled > 1U || fired > 1U) return reject("replicated trigger flags are malformed");
        trigger.enabled = enabled != 0U;
        trigger.fired = fired != 0U;
    }
    for (auto& edit : frame.destructionEdits) {
        if (!read_le(bytes, cursor, edit.object) || !read_le(bytes, cursor, edit.editSequence) ||
            !read_le(bytes, cursor, edit.center.xMillimeters) ||
            !read_le(bytes, cursor, edit.center.yMillimeters) ||
            !read_le(bytes, cursor, edit.center.zMillimeters) ||
            !read_le(bytes, cursor, edit.radiusMillimeters) ||
            !read_le(bytes, cursor, edit.expectedBrickRevision) ||
            !read_le(bytes, cursor, edit.expectedContentHash))
            return reject("replicated destruction edit is truncated");
        if (version >= kCurrentVersion &&
            (!read_le(bytes, cursor, edit.brick.x) || !read_le(bytes, cursor, edit.brick.y) ||
             !read_le(bytes, cursor, edit.brick.z)))
            return reject("replicated destruction brick address is truncated");
    }
    if (cursor != bytes.size()) return reject("replication frame has trailing bytes");
    if (!frame.validate(error)) return std::nullopt;
    return frame;
}

} // namespace dve
