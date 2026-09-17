#include "dve/network_runtime_services.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "dve/transform.hpp"

namespace dve {
namespace {

constexpr std::uint32_t kLateJoinMagic = 0x43455644U; // DVEC little endian
constexpr std::uint16_t kLateJoinVersion = 1U;

template <typename T>
void append_le(std::vector<std::uint8_t>& output, T value) {
    using U = std::make_unsigned_t<T>;
    U unsignedValue = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        output.push_back(static_cast<std::uint8_t>(unsignedValue >> (i * 8U)));
}

template <typename T>
bool read_le(std::span<const std::uint8_t> bytes, std::size_t& cursor, T& value) noexcept {
    if (cursor + sizeof(T) > bytes.size()) return false;
    using U = std::make_unsigned_t<T>;
    std::uint64_t result{};
    for (std::size_t i = 0; i < sizeof(T); ++i)
        result |= static_cast<std::uint64_t>(bytes[cursor++]) << (i * 8U);
    value = static_cast<T>(static_cast<U>(result));
    return true;
}

bool digest_is_zero(const NetworkSha256Digest& digest) noexcept {
    return std::all_of(digest.begin(), digest.end(), [](std::uint8_t value) { return value == 0U; });
}

Float3 lerp_float3(Float3 a, Float3 b, float alpha) noexcept {
    return add(a, multiply(subtract(b, a), alpha));
}

RemoteCharacterSample make_sample(
    double renderTick, const ReplicatedCharacterState& state) noexcept {
    RemoteCharacterSample sample;
    sample.pawn = state.pawn;
    sample.renderTick = renderTick;
    sample.position = dequantize_position_meters(state.position);
    sample.velocity = dequantize_velocity_meters(state.velocity);
    sample.grounded = (state.flags & 0x01U) != 0U;
    sample.crouched = (state.flags & 0x02U) != 0U;
    sample.support = state.support;
    return sample;
}

bool is_delta_kind(ReplicationMessageKind kind) noexcept {
    return kind == ReplicationMessageKind::GameplaySnapshot ||
           kind == ReplicationMessageKind::DestructionBatch ||
           kind == ReplicationMessageKind::SpawnDespawn ||
           kind == ReplicationMessageKind::BrickRepairResponse;
}

} // namespace

bool ServerInputValidationConfig::validate(std::string* error) const noexcept {
    if (maximumPastTicks == 0U || maximumFutureTicks == 0U ||
        maximumCommandsPerServerTick == 0U || !std::isfinite(maximumMoveMagnitude) ||
        maximumMoveMagnitude < 1.0F || maximumMoveMagnitude > 1.25F) {
        if (error) *error = "server input validation limits are invalid";
        return false;
    }
    return true;
}

ServerInputValidator::ServerInputValidator(ServerInputValidationConfig config)
    : config_(std::move(config)) {
    if (!config_.validate()) config_ = {};
}

bool ServerInputValidator::bind_owned_pawn(
    ReplicationPeerId peer, NetworkObjectId pawn, std::string* error) {
    if (peer == kInvalidReplicationPeerId || pawn == kInvalidNetworkObjectId) {
        if (error) *error = "input ownership requires valid peer and pawn ids";
        return false;
    }
    for (const auto& [existingPeer, state] : peers_) {
        if (existingPeer != peer && state.pawn == pawn) {
            if (error) *error = "the network pawn is already owned by another peer";
            return false;
        }
    }
    PeerState& state = peers_[peer];
    state = {};
    state.pawn = pawn;
    return true;
}

bool ServerInputValidator::unbind_peer(ReplicationPeerId peer) { return peers_.erase(peer) != 0U; }

std::optional<NetworkObjectId> ServerInputValidator::owned_pawn(ReplicationPeerId peer) const noexcept {
    const auto iterator = peers_.find(peer);
    if (iterator == peers_.end()) return std::nullopt;
    return iterator->second.pawn;
}

ServerInputValidationResult ServerInputValidator::accept(
    ReplicationPeerId peer, std::uint64_t serverTick,
    const ReplicatedInputCommand& command) noexcept {
    ServerInputValidationResult result;
    result.clientTick = command.clientTick;
    const auto iterator = peers_.find(peer);
    if (iterator == peers_.end()) { result.rejection = ServerInputRejection::UnknownPeer; return result; }
    PeerState& state = iterator->second;
    if (command.pawn != state.pawn) { result.rejection = ServerInputRejection::PawnNotOwned; return result; }
    if (!command.validate() || command.clientTick == 0U) {
        result.rejection = ServerInputRejection::InvalidCommand; return result;
    }
    if (serverTick > command.clientTick && serverTick - command.clientTick > config_.maximumPastTicks) {
        result.rejection = ServerInputRejection::TickTooOld; return result;
    }
    if (command.clientTick > serverTick && command.clientTick - serverTick > config_.maximumFutureTicks) {
        result.rejection = ServerInputRejection::TickTooFarAhead; return result;
    }
    if (command.clientTick <= state.lastClientTick) {
        result.rejection = ServerInputRejection::NonMonotonicTick; return result;
    }
    const CharacterInput decoded = decode_replicated_input(command);
    const float magnitudeSquared = decoded.move.x * decoded.move.x + decoded.move.y * decoded.move.y;
    if (!std::isfinite(magnitudeSquared) ||
        magnitudeSquared > config_.maximumMoveMagnitude * config_.maximumMoveMagnitude) {
        result.rejection = ServerInputRejection::MovementOutOfRange; return result;
    }
    if (state.commandServerTick != serverTick) {
        state.commandServerTick = serverTick;
        state.commandsThisServerTick = 0U;
    }
    if (state.commandsThisServerTick >= config_.maximumCommandsPerServerTick) {
        result.rejection = ServerInputRejection::RateLimited; return result;
    }
    if (decoded.jumpPressed && state.lastJumpTick != 0U &&
        command.clientTick - state.lastJumpTick < config_.minimumJumpIntervalTicks) {
        result.rejection = ServerInputRejection::JumpRateLimited; return result;
    }
    ++state.commandsThisServerTick;
    state.lastClientTick = command.clientTick;
    if (decoded.jumpPressed) state.lastJumpTick = command.clientTick;
    result.accepted = true;
    result.rejection = ServerInputRejection::NoRejection;
    result.input = decoded;
    return result;
}

void ServerInputValidator::reset() noexcept { peers_.clear(); }

ServerInputExecutionResult validate_and_apply_server_input(
    GameWorld& world, const ReplicationIdentityRegistry& identities,
    ServerInputValidator& validator, ReplicationPeerId peer, std::uint64_t serverTick,
    const ReplicatedInputCommand& command, std::string* error) {
    ServerInputExecutionResult result;
    result.validation = validator.accept(peer, serverTick, command);
    if (!result.validation.accepted) return result;
    const auto localPawn = identities.local_object(command.pawn);
    if (!localPawn || !world.gameplay().has_character(*localPawn)) {
        if (error) *error = "validated input pawn is not bound to a live character";
        return result;
    }
    result.localPawn = *localPawn;
    result.applied = world.gameplay().set_character_input(*localPawn, result.validation.input);
    if (!result.applied && error) *error = "validated input could not be applied to the character";
    return result;
}

bool RemoteInterpolationConfig::validate(std::string* error) const noexcept {
    if (!std::isfinite(interpolationDelayTicks) || interpolationDelayTicks < 0.0 ||
        !std::isfinite(maximumExtrapolationTicks) || maximumExtrapolationTicks < 0.0 ||
        !std::isfinite(secondsPerTick) || secondsPerTick <= 0.0 || secondsPerTick > 1.0 ||
        !std::isfinite(teleportDistanceMeters) || teleportDistanceMeters <= 0.0F ||
        maximumSnapshots < 2U || maximumSnapshots > 4096U) {
        if (error) *error = "remote interpolation limits are invalid";
        return false;
    }
    return true;
}

RemoteCharacterInterpolator::RemoteCharacterInterpolator(RemoteInterpolationConfig config)
    : config_(std::move(config)) {
    if (!config_.validate()) config_ = {};
}

bool RemoteCharacterInterpolator::push(
    std::uint64_t serverTick, const ReplicatedCharacterState& state, std::string* error) {
    if (serverTick == 0U || state.pawn == kInvalidNetworkObjectId ||
        (!snapshots_.empty() && state.pawn != snapshots_.front().state.pawn)) {
        if (error) *error = "remote interpolation snapshot is invalid";
        return false;
    }
    if (!snapshots_.empty() && serverTick <= snapshots_.back().serverTick) {
        if (error) *error = "remote interpolation rejected a stale snapshot";
        return false;
    }
    snapshots_.push_back({serverTick, state});
    while (snapshots_.size() > config_.maximumSnapshots) snapshots_.pop_front();
    return true;
}

std::optional<RemoteCharacterSample> RemoteCharacterInterpolator::sample(
    double estimatedServerTick) const noexcept {
    if (snapshots_.empty() || !std::isfinite(estimatedServerTick)) return std::nullopt;
    const double renderTick = estimatedServerTick - config_.interpolationDelayTicks;
    if (snapshots_.size() == 1U || renderTick <= static_cast<double>(snapshots_.front().serverTick))
        return make_sample(renderTick, snapshots_.front().state);
    for (std::size_t index = 1U; index < snapshots_.size(); ++index) {
        const Snapshot& right = snapshots_[index];
        if (renderTick > static_cast<double>(right.serverTick)) continue;
        const Snapshot& left = snapshots_[index - 1U];
        const auto leftSample = make_sample(renderTick, left.state);
        auto rightSample = make_sample(renderTick, right.state);
        if (length(subtract(rightSample.position, leftSample.position)) > config_.teleportDistanceMeters) {
            rightSample.teleportSnap = true;
            return rightSample;
        }
        const double span = static_cast<double>(right.serverTick - left.serverTick);
        const float alpha = span <= 0.0 ? 1.0F : static_cast<float>(std::clamp(
            (renderTick - static_cast<double>(left.serverTick)) / span, 0.0, 1.0));
        RemoteCharacterSample sample = alpha < 0.5F ? leftSample : rightSample;
        sample.renderTick = renderTick;
        sample.position = lerp_float3(leftSample.position, rightSample.position, alpha);
        sample.velocity = lerp_float3(leftSample.velocity, rightSample.velocity, alpha);
        return sample;
    }
    RemoteCharacterSample sample = make_sample(renderTick, snapshots_.back().state);
    const double extrapolationTicks = std::clamp(
        renderTick - static_cast<double>(snapshots_.back().serverTick),
        0.0, config_.maximumExtrapolationTicks);
    if (extrapolationTicks > 0.0) {
        sample.position = add(sample.position, multiply(sample.velocity,
            static_cast<float>(extrapolationTicks * config_.secondsPerTick)));
        sample.extrapolated = true;
    }
    return sample;
}

bool apply_remote_character_sample(
    GameWorld& world, const ReplicationIdentityRegistry& identities,
    const RemoteCharacterSample& sample, std::string* error) {
    const auto localPawn = identities.local_object(sample.pawn);
    if (!localPawn || !world.gameplay().has_character(*localPawn)) {
        if (error) *error = "remote sample pawn is not bound to a live character";
        return false;
    }
    GameObjectId support = kInvalidGameObjectId;
    if (sample.support != kInvalidNetworkObjectId) {
        if (const auto localSupport = identities.local_object(sample.support)) support = *localSupport;
    }
    const auto tick = sample.renderTick <= 0.0 ? 0U : static_cast<std::uint64_t>(sample.renderTick);
    return world.gameplay().apply_authoritative_character_state(
        *localPawn, sample.position, sample.velocity, sample.grounded, support,
        sample.crouched ? CharacterStance::Crouched : CharacterStance::Standing,
        tick, error);
}

bool LateJoinCheckpointManifest::validate(std::string* error) const noexcept {
    if (checkpointId == 0U || serverTick == 0U || totalBytes == 0U || chunkBytes == 0U ||
        chunkCount == 0U || chunkCount > 8192U || digest_is_zero(contentHash) ||
        static_cast<std::uint64_t>(chunkBytes) * chunkCount < totalBytes ||
        totalBytes <= static_cast<std::uint64_t>(chunkBytes) * (chunkCount - 1U)) {
        if (error) *error = "late-join checkpoint manifest is invalid";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_late_join_checkpoint_packet(
    const LateJoinCheckpointPacket& packet, std::string* error) {
    std::vector<std::uint8_t> output;
    append_le(output, kLateJoinMagic);
    append_le(output, kLateJoinVersion);
    output.push_back(static_cast<std::uint8_t>(packet.type));
    output.push_back(0U);
    if (packet.type == LateJoinCheckpointPacketType::Begin) {
        if (!packet.manifest.validate(error)) return {};
        append_le(output, packet.manifest.checkpointId);
        append_le(output, packet.manifest.serverTick);
        append_le(output, packet.manifest.baselineEditSequence);
        append_le(output, packet.manifest.totalBytes);
        append_le(output, packet.manifest.chunkBytes);
        append_le(output, packet.manifest.chunkCount);
        output.insert(output.end(), packet.manifest.contentHash.begin(), packet.manifest.contentHash.end());
    } else if (packet.type == LateJoinCheckpointPacketType::Chunk) {
        if (packet.manifest.checkpointId == 0U || packet.chunk.empty() ||
            packet.chunk.size() > std::numeric_limits<std::uint32_t>::max()) {
            if (error) *error = "late-join checkpoint chunk is invalid";
            return {};
        }
        append_le(output, packet.manifest.checkpointId);
        append_le(output, packet.chunkIndex);
        append_le(output, static_cast<std::uint32_t>(packet.chunk.size()));
        output.insert(output.end(), packet.chunk.begin(), packet.chunk.end());
    } else if (packet.type == LateJoinCheckpointPacketType::Commit) {
        if (packet.manifest.checkpointId == 0U) {
            if (error) *error = "late-join checkpoint commit is invalid";
            return {};
        }
        append_le(output, packet.manifest.checkpointId);
    } else {
        if (error) *error = "late-join checkpoint packet type is unsupported";
        return {};
    }
    return output;
}

std::optional<LateJoinCheckpointPacket> decode_late_join_checkpoint_packet(
    std::span<const std::uint8_t> bytes, std::string* error) {
    const auto reject = [&](const char* message) -> std::optional<LateJoinCheckpointPacket> {
        if (error) *error = message;
        return std::nullopt;
    };
    std::size_t cursor{};
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint8_t type{}, reserved{};
    if (!read_le(bytes, cursor, magic) || !read_le(bytes, cursor, version) ||
        !read_le(bytes, cursor, type) || !read_le(bytes, cursor, reserved) ||
        magic != kLateJoinMagic || version != kLateJoinVersion || reserved != 0U ||
        type < static_cast<std::uint8_t>(LateJoinCheckpointPacketType::Begin) ||
        type > static_cast<std::uint8_t>(LateJoinCheckpointPacketType::Commit))
        return reject("late-join checkpoint packet header is invalid");
    LateJoinCheckpointPacket packet;
    packet.type = static_cast<LateJoinCheckpointPacketType>(type);
    if (packet.type == LateJoinCheckpointPacketType::Begin) {
        if (!read_le(bytes, cursor, packet.manifest.checkpointId) ||
            !read_le(bytes, cursor, packet.manifest.serverTick) ||
            !read_le(bytes, cursor, packet.manifest.baselineEditSequence) ||
            !read_le(bytes, cursor, packet.manifest.totalBytes) ||
            !read_le(bytes, cursor, packet.manifest.chunkBytes) ||
            !read_le(bytes, cursor, packet.manifest.chunkCount) ||
            cursor + packet.manifest.contentHash.size() != bytes.size())
            return reject("late-join checkpoint manifest is truncated");
        std::copy_n(bytes.data() + cursor, packet.manifest.contentHash.size(),
                    packet.manifest.contentHash.begin());
        cursor += packet.manifest.contentHash.size();
        if (!packet.manifest.validate(error)) return std::nullopt;
    } else if (packet.type == LateJoinCheckpointPacketType::Chunk) {
        std::uint32_t chunkBytes{};
        if (!read_le(bytes, cursor, packet.manifest.checkpointId) ||
            !read_le(bytes, cursor, packet.chunkIndex) || !read_le(bytes, cursor, chunkBytes) ||
            chunkBytes == 0U || cursor + chunkBytes != bytes.size())
            return reject("late-join checkpoint chunk is truncated");
        packet.chunk.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
        cursor = bytes.size();
    } else {
        if (!read_le(bytes, cursor, packet.manifest.checkpointId) || cursor != bytes.size() ||
            packet.manifest.checkpointId == 0U)
            return reject("late-join checkpoint commit is invalid");
    }
    if (cursor != bytes.size()) return reject("late-join checkpoint packet has trailing bytes");
    return packet;
}

bool LateJoinCheckpointSender::begin(
    std::uint64_t checkpointId, std::uint64_t serverTick,
    std::uint64_t baselineEditSequence, std::span<const std::uint8_t> checkpoint,
    std::uint32_t chunkBytes, std::string* error) {
    if (checkpointId == 0U || serverTick == 0U || checkpoint.empty() || chunkBytes == 0U ||
        checkpoint.size() > 64U * 1024U * 1024U) {
        if (error) *error = "late-join checkpoint source is invalid";
        return false;
    }
    reset();
    checkpoint_.assign(checkpoint.begin(), checkpoint.end());
    manifest_.checkpointId = checkpointId;
    manifest_.serverTick = serverTick;
    manifest_.baselineEditSequence = baselineEditSequence;
    manifest_.totalBytes = checkpoint_.size();
    manifest_.chunkBytes = chunkBytes;
    manifest_.chunkCount = static_cast<std::uint32_t>(
        (checkpoint_.size() + chunkBytes - 1U) / chunkBytes);
    manifest_.contentHash = network_sha256(checkpoint_);
    if (!manifest_.validate(error)) { reset(); return false; }
    return true;
}

std::size_t LateJoinCheckpointSender::pump(
    ReplicationTransportEndpoint& endpoint, ReplicationPeerId destination,
    std::size_t maximumChunks, std::string* error) {
    if (manifest_.checkpointId == 0U || commitSent_ || maximumChunks == 0U) return 0U;
    std::size_t sent{};
    if (!beginSent_) {
        LateJoinCheckpointPacket packet;
        packet.type = LateJoinCheckpointPacketType::Begin;
        packet.manifest = manifest_;
        const auto bytes = encode_late_join_checkpoint_packet(packet, error);
        if (bytes.empty() || !endpoint.send(destination, ReplicationTransportChannel::ReliableOrdered,
                                            ReplicationMessageKind::CheckpointFragment,
                                            bytes, error)) return sent;
        beginSent_ = true;
    }
    while (nextChunk_ < manifest_.chunkCount && sent < maximumChunks) {
        const std::size_t offset = static_cast<std::size_t>(nextChunk_) * manifest_.chunkBytes;
        const std::size_t count = std::min<std::size_t>(manifest_.chunkBytes, checkpoint_.size() - offset);
        LateJoinCheckpointPacket packet;
        packet.type = LateJoinCheckpointPacketType::Chunk;
        packet.manifest.checkpointId = manifest_.checkpointId;
        packet.chunkIndex = nextChunk_;
        packet.chunk.assign(checkpoint_.begin() + static_cast<std::ptrdiff_t>(offset),
                            checkpoint_.begin() + static_cast<std::ptrdiff_t>(offset + count));
        const auto bytes = encode_late_join_checkpoint_packet(packet, error);
        if (bytes.empty() || !endpoint.send(destination, ReplicationTransportChannel::ReliableOrdered,
                                            ReplicationMessageKind::CheckpointFragment,
                                            bytes, error)) return sent;
        ++nextChunk_;
        ++sent;
    }
    if (nextChunk_ == manifest_.chunkCount && !commitSent_) {
        LateJoinCheckpointPacket packet;
        packet.type = LateJoinCheckpointPacketType::Commit;
        packet.manifest.checkpointId = manifest_.checkpointId;
        const auto bytes = encode_late_join_checkpoint_packet(packet, error);
        if (!bytes.empty() && endpoint.send(destination, ReplicationTransportChannel::ReliableOrdered,
                                             ReplicationMessageKind::CheckpointFragment,
                                             bytes, error)) commitSent_ = true;
    }
    return sent;
}

void LateJoinCheckpointSender::reset() noexcept {
    manifest_ = {};
    checkpoint_.clear();
    nextChunk_ = 0U;
    beginSent_ = false;
    commitSent_ = false;
}

bool LateJoinReceiverConfig::validate(std::string* error) const noexcept {
    if (maximumCheckpointBytes == 0U || maximumCheckpointBytes > 512U * 1024U * 1024U ||
        maximumChunks == 0U || maximumChunks > 65536U ||
        maximumBufferedDeltaMessages == 0U || maximumBufferedDeltaBytes == 0U) {
        if (error) *error = "late-join receiver limits are invalid";
        return false;
    }
    return true;
}

LateJoinCheckpointReceiver::LateJoinCheckpointReceiver(LateJoinReceiverConfig config)
    : config_(std::move(config)) {
    if (!config_.validate()) config_ = {};
}

bool LateJoinCheckpointReceiver::accept(
    const ReplicationTransportMessage& message, std::string* error) {
    if (message.kind != ReplicationMessageKind::CheckpointFragment) {
        if (error) *error = "late-join receiver requires checkpoint-fragment messages";
        return false;
    }
    const auto packet = decode_late_join_checkpoint_packet(message.payload, error);
    if (!packet) return false;
    if (packet->type == LateJoinCheckpointPacketType::Begin) {
        if (packet->manifest.totalBytes > config_.maximumCheckpointBytes ||
            packet->manifest.chunkCount > config_.maximumChunks) {
            if (error) *error = "late-join checkpoint exceeds receiver limits";
            return false;
        }
        if (manifest_) {
            if (manifest_->checkpointId == packet->manifest.checkpointId &&
                manifest_->contentHash == packet->manifest.contentHash) return true;
            if (error) *error = "late-join receiver already has a different checkpoint";
            return false;
        }
        manifest_ = packet->manifest;
        chunks_.resize(manifest_->chunkCount);
        received_.assign(manifest_->chunkCount, false);
        return true;
    }
    if (!manifest_ || packet->manifest.checkpointId != manifest_->checkpointId) {
        if (error) *error = "late-join packet does not match the active checkpoint";
        return false;
    }
    if (packet->type == LateJoinCheckpointPacketType::Chunk) {
        if (packet->chunkIndex >= manifest_->chunkCount) {
            if (error) *error = "late-join checkpoint chunk index is out of range";
            return false;
        }
        const std::size_t expected = packet->chunkIndex + 1U == manifest_->chunkCount
            ? static_cast<std::size_t>(manifest_->totalBytes) -
                static_cast<std::size_t>(packet->chunkIndex) * manifest_->chunkBytes
            : manifest_->chunkBytes;
        if (packet->chunk.size() != expected) {
            if (error) *error = "late-join checkpoint chunk has the wrong size";
            return false;
        }
        if (received_[packet->chunkIndex]) {
            if (chunks_[packet->chunkIndex] != packet->chunk) {
                if (error) *error = "late-join duplicate chunk content differs";
                return false;
            }
            return true;
        }
        chunks_[packet->chunkIndex] = packet->chunk;
        received_[packet->chunkIndex] = true;
        receivedBytes_ += packet->chunk.size();
    } else {
        commitReceived_ = true;
    }
    if (commitReceived_ && std::all_of(received_.begin(), received_.end(), [](bool value) { return value; })) {
        assembled_.clear();
        assembled_.reserve(static_cast<std::size_t>(manifest_->totalBytes));
        for (const auto& chunk : chunks_) assembled_.insert(assembled_.end(), chunk.begin(), chunk.end());
        if (assembled_.size() != manifest_->totalBytes || network_sha256(assembled_) != manifest_->contentHash) {
            assembled_.clear();
            if (error) *error = "late-join checkpoint content hash does not match";
            return false;
        }
        complete_ = true;
    }
    return true;
}

bool LateJoinCheckpointReceiver::buffer_delta(
    const ReplicationTransportMessage& message, std::string* error) {
    if (!manifest_ || complete_ || !is_delta_kind(message.kind)) {
        if (error) *error = "late-join delta cannot be buffered in the current state";
        return false;
    }
    if (bufferedDeltas_.size() >= config_.maximumBufferedDeltaMessages ||
        bufferedDeltaBytes_ + message.payload.size() > config_.maximumBufferedDeltaBytes) {
        if (error) *error = "late-join delta buffer limit exceeded";
        return false;
    }
    bufferedDeltaBytes_ += message.payload.size();
    bufferedDeltas_.push_back(message);
    return true;
}

double LateJoinCheckpointReceiver::progress() const noexcept {
    if (!manifest_) return 0.0;
    if (complete_) return 1.0;
    return static_cast<double>(receivedBytes_) / static_cast<double>(manifest_->totalBytes);
}

const LateJoinCheckpointManifest* LateJoinCheckpointReceiver::manifest() const noexcept {
    return manifest_ ? &*manifest_ : nullptr;
}

std::optional<std::vector<std::uint8_t>> LateJoinCheckpointReceiver::take_checkpoint() {
    if (!complete_) return std::nullopt;
    complete_ = false;
    return std::exchange(assembled_, {});
}

std::vector<ReplicationTransportMessage> LateJoinCheckpointReceiver::take_buffered_deltas() {
    bufferedDeltaBytes_ = 0U;
    return std::exchange(bufferedDeltas_, {});
}

void LateJoinCheckpointReceiver::reset() noexcept {
    manifest_.reset();
    chunks_.clear();
    received_.clear();
    receivedBytes_ = 0U;
    commitReceived_ = false;
    complete_ = false;
    assembled_.clear();
    bufferedDeltas_.clear();
    bufferedDeltaBytes_ = 0U;
}

} // namespace dve
