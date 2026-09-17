#include "dve/network_simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace dve {
namespace {

constexpr std::size_t kDatagramOverheadBytes = 56U;
constexpr std::uint32_t kInputMagic = 0x31495644U; // DVI1
constexpr std::uint32_t kRepairMagic = 0x31524244U; // DBR1
constexpr std::uint32_t kRepairResponseMagic = 0x31505244U; // DRP1
constexpr std::uint16_t kCodecVersion = 1U;

template <typename T>
void append_le(std::vector<std::uint8_t>& output, T value) {
    using U = std::make_unsigned_t<T>;
    U bits{};
    std::memcpy(&bits, &value, sizeof(value));
    for (std::size_t index = 0; index < sizeof(T); ++index)
        output.push_back(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xFFU));
}

template <typename T>
bool read_le(std::span<const std::uint8_t> input, std::size_t& cursor, T& value) {
    if (cursor + sizeof(T) > input.size()) return false;
    using U = std::make_unsigned_t<T>;
    U bits{};
    for (std::size_t index = 0; index < sizeof(T); ++index)
        bits = static_cast<U>(bits | static_cast<U>(input[cursor + index]) << (index * 8U));
    std::memcpy(&value, &bits, sizeof(value));
    cursor += sizeof(T);
    return true;
}

[[nodiscard]] std::int16_t quantize_input_axis(float value) noexcept {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0F));
}

struct LinkKey {
    SimulatedPeerId source{};
    SimulatedPeerId destination{};
    auto operator<=>(const LinkKey&) const = default;
};

struct StreamKey {
    SimulatedPeerId source{};
    SimulatedPeerId destination{};
    ReplicationTransportChannel channel{};
    auto operator<=>(const StreamKey&) const = default;
};

struct ReassemblyKey {
    SimulatedPeerId source{};
    SimulatedPeerId destination{};
    ReplicationTransportChannel channel{};
    std::uint64_t sequence{};
    auto operator<=>(const ReassemblyKey&) const = default;
};

} // namespace

bool SimulatedNetworkConfig::validate(std::string* error) const noexcept {
    const auto reject = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (minimumLatencyTicks > maximumLatencyTicks)
        return reject("minimum network latency cannot exceed maximum latency");
    if (lossPermille > 1000U || duplicationPermille > 1000U || reorderPermille > 1000U)
        return reject("network impairment probabilities must be in [0,1000]");
    if (maximumDatagramBytes <= kDatagramOverheadBytes + 8U)
        return reject("maximum datagram size is too small for framing");
    if (bandwidthBytesPerTick < maximumDatagramBytes)
        return reject("per-tick bandwidth must fit at least one maximum-sized datagram");
    if (maximumMessageBytes == 0U || maximumMessageBytes > 64U * 1024U * 1024U)
        return reject("maximum logical message size is outside the supported range");
    if (maximumInFlightReliableMessages == 0U || maximumReassemblies == 0U)
        return reject("network queue limits must be non-zero");
    if (reliableRetryTicks == 0U || maximumReliableAttempts == 0U || reassemblyTimeoutTicks == 0U)
        return reject("network retry and timeout values must be non-zero");
    return true;
}

struct DeterministicReplicationTransport::Impl {
    struct Datagram {
        SimulatedPeerId source{};
        SimulatedPeerId destination{};
        ReplicationTransportChannel channel{};
        ReplicationMessageKind kind{};
        std::uint64_t sequence{};
        std::uint16_t fragmentIndex{};
        std::uint16_t fragmentCount{1U};
        bool acknowledgement{};
        std::vector<std::uint8_t> payload;
        std::uint64_t order{};

        [[nodiscard]] std::size_t wire_bytes() const noexcept {
            return kDatagramOverheadBytes + payload.size();
        }
    };

    struct ScheduledDatagram {
        std::uint64_t deliveryTick{};
        Datagram datagram;
    };

    struct PendingReliable {
        ReplicationMessageKind kind{};
        std::vector<std::uint8_t> payload;
        std::uint64_t lastTransmitTick{std::numeric_limits<std::uint64_t>::max()};
        std::uint32_t attempts{};
    };

    struct Reassembly {
        ReplicationMessageKind kind{};
        std::uint16_t fragmentCount{};
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> received;
        std::size_t receivedCount{};
        std::size_t totalBytes{};
        std::uint64_t firstTick{};
    };

    explicit Impl(SimulatedNetworkConfig input) : config(std::move(input)), randomState(config.seed) {}

    [[nodiscard]] std::uint64_t random_u64() noexcept {
        std::uint64_t value = randomState;
        if (value == 0U) value = 0x9E3779B97F4A7C15ULL;
        value ^= value >> 12U;
        value ^= value << 25U;
        value ^= value >> 27U;
        randomState = value;
        return value * 2685821657736338717ULL;
    }

    [[nodiscard]] std::uint32_t random_inclusive(std::uint32_t minimum, std::uint32_t maximum) noexcept {
        if (minimum >= maximum) return minimum;
        const std::uint64_t range = static_cast<std::uint64_t>(maximum) - minimum + 1U;
        return minimum + static_cast<std::uint32_t>(random_u64() % range);
    }

    [[nodiscard]] bool chance(std::uint16_t permille) noexcept {
        return permille > 0U && (random_u64() % 1000U) < permille;
    }

    [[nodiscard]] std::size_t payload_capacity() const noexcept {
        return config.maximumDatagramBytes - kDatagramOverheadBytes;
    }

    [[nodiscard]] std::uint64_t next_sequence(const StreamKey& key) {
        std::uint64_t& next = nextSequences[key];
        if (next == 0U) next = 1U;
        return next++;
    }

    void schedule_one(Datagram datagram, bool duplicate = false) {
        ++telemetry.datagramsAttempted;
        telemetry.bytesAttempted += datagram.wire_bytes();
        if (chance(config.lossPermille)) {
            ++telemetry.datagramsDropped;
            return;
        }
        std::uint32_t latency = random_inclusive(config.minimumLatencyTicks, config.maximumLatencyTicks);
        if (chance(config.reorderPermille) && config.reorderExtraTicks > 0U)
            latency += random_inclusive(1U, config.reorderExtraTicks);
        const bool shouldDuplicate = !duplicate && chance(config.duplicationPermille);
        Datagram duplicateCopy;
        if (shouldDuplicate) duplicateCopy = datagram;
        datagram.order = nextDatagramOrder++;
        scheduled.emplace(std::make_pair(currentTick + latency, datagram.order),
                          ScheduledDatagram{currentTick + latency, std::move(datagram)});
        ++telemetry.datagramsScheduled;
        if (shouldDuplicate) {
            ++telemetry.datagramsDuplicated;
            schedule_one(std::move(duplicateCopy), true);
        }
    }

    void transmit(
        SimulatedPeerId source, SimulatedPeerId destination,
        ReplicationTransportChannel channel, ReplicationMessageKind kind,
        std::uint64_t sequence, std::span<const std::uint8_t> payload,
        bool retransmission) {
        const std::size_t capacity = payload_capacity();
        const std::size_t count = std::max<std::size_t>(1U, (payload.size() + capacity - 1U) / capacity);
        if (count > std::numeric_limits<std::uint16_t>::max()) return;
        for (std::size_t fragment = 0U; fragment < count; ++fragment) {
            const std::size_t begin = fragment * capacity;
            const std::size_t end = std::min(payload.size(), begin + capacity);
            Datagram datagram;
            datagram.source = source;
            datagram.destination = destination;
            datagram.channel = channel;
            datagram.kind = kind;
            datagram.sequence = sequence;
            datagram.fragmentIndex = static_cast<std::uint16_t>(fragment);
            datagram.fragmentCount = static_cast<std::uint16_t>(count);
            if (begin < end) datagram.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(begin),
                                                     payload.begin() + static_cast<std::ptrdiff_t>(end));
            schedule_one(std::move(datagram));
        }
        if (retransmission) ++telemetry.reliableRetransmissions;
    }

    void send_ack(SimulatedPeerId receiver, SimulatedPeerId sender, std::uint64_t sequence) {
        Datagram datagram;
        datagram.source = receiver;
        datagram.destination = sender;
        datagram.channel = ReplicationTransportChannel::ReliableOrdered;
        datagram.sequence = sequence;
        datagram.acknowledgement = true;
        schedule_one(std::move(datagram));
    }

    void deliver_message(
        SimulatedPeerId source, SimulatedPeerId destination,
        ReplicationTransportChannel channel, ReplicationMessageKind kind,
        std::uint64_t sequence, std::vector<std::uint8_t> payload) {
        inbox[destination].push_back({source, destination, channel, kind, sequence, currentTick,
                                      std::move(payload)});
        ++telemetry.logicalMessagesDelivered;
    }

    void process_ack(const Datagram& datagram) {
        const LinkKey link{datagram.destination, datagram.source};
        auto pendingLink = pendingReliable.find(link);
        if (pendingLink == pendingReliable.end()) return;
        if (pendingLink->second.erase(datagram.sequence) > 0U)
            ++telemetry.reliableMessagesAcknowledged;
        if (pendingLink->second.empty()) pendingReliable.erase(pendingLink);
    }

    void complete_reassembly(const ReassemblyKey& key, Reassembly& assembly) {
        std::vector<std::uint8_t> payload;
        payload.reserve(assembly.totalBytes);
        for (auto& fragment : assembly.fragments)
            payload.insert(payload.end(), fragment.begin(), fragment.end());

        const StreamKey stream{key.source, key.destination, key.channel};
        if (key.channel == ReplicationTransportChannel::ReliableOrdered) {
            send_ack(key.destination, key.source, key.sequence);
            std::uint64_t& next = nextReliableReceive[stream];
            if (next == 0U) next = 1U;
            if (key.sequence < next) {
                ++telemetry.duplicateReliableMessages;
                return;
            }
            reliableReceiveBuffer[stream][key.sequence] =
                ReplicationTransportMessage{key.source, key.destination, key.channel, assembly.kind,
                                            key.sequence, currentTick, std::move(payload)};
            auto& buffer = reliableReceiveBuffer[stream];
            while (true) {
                auto it = buffer.find(next);
                if (it == buffer.end()) break;
                inbox[key.destination].push_back(std::move(it->second));
                buffer.erase(it);
                ++telemetry.logicalMessagesDelivered;
                ++next;
            }
            if (buffer.empty()) reliableReceiveBuffer.erase(stream);
        } else {
            std::uint64_t& last = lastUnreliableReceive[stream];
            if (key.sequence <= last) {
                ++telemetry.staleUnreliableDatagrams;
                return;
            }
            last = key.sequence;
            deliver_message(key.source, key.destination, key.channel, assembly.kind,
                            key.sequence, std::move(payload));
            for (auto it = reassemblies.begin(); it != reassemblies.end();) {
                if (it->first.source == key.source && it->first.destination == key.destination &&
                    it->first.channel == key.channel && it->first.sequence < key.sequence)
                    it = reassemblies.erase(it);
                else
                    ++it;
            }
        }
    }

    void process_data(Datagram datagram) {
        if (datagram.fragmentCount == 0U || datagram.fragmentIndex >= datagram.fragmentCount) return;
        const StreamKey stream{datagram.source, datagram.destination, datagram.channel};
        if (datagram.channel == ReplicationTransportChannel::ReliableOrdered) {
            std::uint64_t& next = nextReliableReceive[stream];
            if (next == 0U) next = 1U;
            if (datagram.sequence < next) {
                send_ack(datagram.destination, datagram.source, datagram.sequence);
                ++telemetry.duplicateReliableMessages;
                return;
            }
        } else {
            const std::uint64_t last = lastUnreliableReceive[stream];
            if (datagram.sequence <= last) {
                ++telemetry.staleUnreliableDatagrams;
                return;
            }
        }

        const ReassemblyKey key{datagram.source, datagram.destination, datagram.channel,
                                datagram.sequence};
        auto it = reassemblies.find(key);
        if (it == reassemblies.end()) {
            if (reassemblies.size() >= config.maximumReassemblies) {
                ++telemetry.datagramsDropped;
                return;
            }
            Reassembly assembly;
            assembly.kind = datagram.kind;
            assembly.fragmentCount = datagram.fragmentCount;
            assembly.fragments.resize(datagram.fragmentCount);
            assembly.received.resize(datagram.fragmentCount, false);
            assembly.firstTick = currentTick;
            it = reassemblies.emplace(key, std::move(assembly)).first;
            telemetry.peakReassemblies = std::max<std::uint64_t>(
                telemetry.peakReassemblies, reassemblies.size());
        }
        Reassembly& assembly = it->second;
        if (assembly.fragmentCount != datagram.fragmentCount || assembly.kind != datagram.kind) {
            reassemblies.erase(it);
            ++telemetry.datagramsDropped;
            return;
        }
        if (!assembly.received[datagram.fragmentIndex]) {
            if (assembly.totalBytes + datagram.payload.size() > config.maximumMessageBytes) {
                reassemblies.erase(it);
                ++telemetry.datagramsDropped;
                return;
            }
            assembly.received[datagram.fragmentIndex] = true;
            assembly.fragments[datagram.fragmentIndex] = std::move(datagram.payload);
            assembly.totalBytes += assembly.fragments[datagram.fragmentIndex].size();
            ++assembly.receivedCount;
        }
        if (assembly.receivedCount == assembly.fragmentCount) {
            complete_reassembly(key, assembly);
            reassemblies.erase(key);
        }
    }

    void process_datagram(Datagram datagram) {
        ++telemetry.datagramsDelivered;
        telemetry.bytesDelivered += datagram.wire_bytes();
        if (datagram.acknowledgement) process_ack(datagram);
        else process_data(std::move(datagram));
    }

    void retransmit_due() {
        for (auto linkIt = pendingReliable.begin(); linkIt != pendingReliable.end();) {
            auto& messages = linkIt->second;
            for (auto messageIt = messages.begin(); messageIt != messages.end();) {
                PendingReliable& pending = messageIt->second;
                const bool first = pending.lastTransmitTick == std::numeric_limits<std::uint64_t>::max();
                const bool due = first || currentTick - pending.lastTransmitTick >= config.reliableRetryTicks;
                if (!due) {
                    ++messageIt;
                    continue;
                }
                if (pending.attempts >= config.maximumReliableAttempts) {
                    ++telemetry.reliableFailures;
                    messageIt = messages.erase(messageIt);
                    continue;
                }
                transmit(linkIt->first.source, linkIt->first.destination,
                         ReplicationTransportChannel::ReliableOrdered, pending.kind,
                         messageIt->first, pending.payload, !first);
                pending.lastTransmitTick = currentTick;
                ++pending.attempts;
                ++messageIt;
            }
            if (messages.empty()) linkIt = pendingReliable.erase(linkIt);
            else ++linkIt;
        }
    }

    void expire_reassemblies() {
        for (auto it = reassemblies.begin(); it != reassemblies.end();) {
            if (currentTick - it->second.firstTick >= config.reassemblyTimeoutTicks) {
                it = reassemblies.erase(it);
                ++telemetry.reassembliesExpired;
            } else {
                ++it;
            }
        }
    }

    SimulatedNetworkConfig config;
    ReplicationTransportTelemetry telemetry;
    std::uint64_t currentTick{};
    std::uint64_t randomState{};
    std::uint64_t nextDatagramOrder{1U};
    std::map<StreamKey, std::uint64_t> nextSequences;
    std::map<LinkKey, std::map<std::uint64_t, PendingReliable>> pendingReliable;
    std::map<StreamKey, std::uint64_t> nextReliableReceive;
    std::map<StreamKey, std::uint64_t> lastUnreliableReceive;
    std::map<StreamKey, std::map<std::uint64_t, ReplicationTransportMessage>> reliableReceiveBuffer;
    std::map<ReassemblyKey, Reassembly> reassemblies;
    std::map<std::pair<std::uint64_t, std::uint64_t>, ScheduledDatagram> scheduled;
    std::map<SimulatedPeerId, std::vector<ReplicationTransportMessage>> inbox;
};

DeterministicReplicationTransport::DeterministicReplicationTransport(SimulatedNetworkConfig config) {
    std::string error;
    if (!config.validate(&error)) config = {};
    impl_ = std::make_unique<Impl>(std::move(config));
}
DeterministicReplicationTransport::~DeterministicReplicationTransport() = default;
DeterministicReplicationTransport::DeterministicReplicationTransport(
    DeterministicReplicationTransport&&) noexcept = default;
DeterministicReplicationTransport& DeterministicReplicationTransport::operator=(
    DeterministicReplicationTransport&&) noexcept = default;

std::optional<std::uint64_t> ReplicationTransportEndpoint::send(
    ReplicationPeerId destination, ReplicationTransportChannel channel,
    ReplicationMessageKind kind, std::span<const std::uint8_t> payload,
    std::string* error) {
    if (transport_ == nullptr || localPeer_ == kInvalidReplicationPeerId ||
        destination == kInvalidReplicationPeerId || destination == localPeer_) {
        if (error != nullptr) *error = "replication endpoint has an invalid source or destination";
        return std::nullopt;
    }
    return transport_->send(localPeer_, destination, channel, kind, payload, error);
}

std::vector<ReplicationTransportMessage> ReplicationTransportEndpoint::receive() {
    if (transport_ == nullptr || localPeer_ == kInvalidReplicationPeerId) return {};
    return transport_->receive(localPeer_);
}

std::optional<std::uint64_t> DeterministicReplicationTransport::send(
    ReplicationPeerId source, ReplicationPeerId destination,
    ReplicationTransportChannel channel, ReplicationMessageKind kind,
    std::span<const std::uint8_t> payload, std::string* error) {
    if (channel == ReplicationTransportChannel::ReliableOrdered)
        return send_reliable(source, destination, kind, payload, error);
    return send_unreliable(source, destination, kind, payload, error);
}

std::optional<std::uint64_t> DeterministicReplicationTransport::send_reliable(
    SimulatedPeerId source, SimulatedPeerId destination, ReplicationMessageKind kind,
    std::span<const std::uint8_t> payload, std::string* error) {
    if (source == kInvalidSimulatedPeerId || destination == kInvalidSimulatedPeerId || source == destination) {
        if (error != nullptr) *error = "network peers must be distinct and non-zero";
        return std::nullopt;
    }
    if (payload.size() > impl_->config.maximumMessageBytes) {
        if (error != nullptr) *error = "reliable message exceeds the configured message limit";
        return std::nullopt;
    }
    std::size_t pendingCount{};
    for (const auto& [link, messages] : impl_->pendingReliable) {
        (void)link;
        pendingCount += messages.size();
    }
    if (pendingCount >= impl_->config.maximumInFlightReliableMessages) {
        if (error != nullptr) *error = "reliable message queue is full";
        return std::nullopt;
    }
    const StreamKey stream{source, destination, ReplicationTransportChannel::ReliableOrdered};
    const std::uint64_t sequence = impl_->next_sequence(stream);
    impl_->pendingReliable[{source, destination}].emplace(
        sequence, Impl::PendingReliable{kind, std::vector<std::uint8_t>(payload.begin(), payload.end())});
    ++impl_->telemetry.logicalMessagesQueued;
    impl_->telemetry.peakPendingReliable = std::max<std::uint64_t>(
        impl_->telemetry.peakPendingReliable, pendingCount + 1U);
    impl_->retransmit_due();
    return sequence;
}

std::optional<std::uint64_t> DeterministicReplicationTransport::send_unreliable(
    SimulatedPeerId source, SimulatedPeerId destination, ReplicationMessageKind kind,
    std::span<const std::uint8_t> payload, std::string* error) {
    if (source == kInvalidSimulatedPeerId || destination == kInvalidSimulatedPeerId || source == destination) {
        if (error != nullptr) *error = "network peers must be distinct and non-zero";
        return std::nullopt;
    }
    if (payload.size() > impl_->config.maximumMessageBytes) {
        if (error != nullptr) *error = "unreliable message exceeds the configured message limit";
        return std::nullopt;
    }
    const StreamKey stream{source, destination, ReplicationTransportChannel::UnreliableSequenced};
    const std::uint64_t sequence = impl_->next_sequence(stream);
    ++impl_->telemetry.logicalMessagesQueued;
    impl_->transmit(source, destination, ReplicationTransportChannel::UnreliableSequenced,
                    kind, sequence, payload, false);
    return sequence;
}

void DeterministicReplicationTransport::advance_tick() {
    ++impl_->currentTick;
    impl_->retransmit_due();
    std::size_t budget = impl_->config.bandwidthBytesPerTick;
    while (!impl_->scheduled.empty()) {
        auto it = impl_->scheduled.begin();
        if (it->second.deliveryTick > impl_->currentTick) break;
        const std::size_t bytes = it->second.datagram.wire_bytes();
        if (bytes > budget) {
            auto node = impl_->scheduled.extract(it);
            node.mapped().deliveryTick = impl_->currentTick + 1U;
            node.key() = {impl_->currentTick + 1U, node.mapped().datagram.order};
            impl_->scheduled.insert(std::move(node));
            break;
        }
        budget -= bytes;
        auto datagram = std::move(it->second.datagram);
        impl_->scheduled.erase(it);
        impl_->process_datagram(std::move(datagram));
    }
    impl_->expire_reassemblies();
}

std::vector<ReplicationTransportMessage> DeterministicReplicationTransport::receive(
    SimulatedPeerId peer) {
    auto it = impl_->inbox.find(peer);
    if (it == impl_->inbox.end()) return {};
    std::vector<ReplicationTransportMessage> result = std::move(it->second);
    impl_->inbox.erase(it);
    return result;
}

std::uint64_t DeterministicReplicationTransport::tick() const noexcept { return impl_->currentTick; }
bool DeterministicReplicationTransport::idle() const noexcept {
    return impl_->pendingReliable.empty() && impl_->scheduled.empty() &&
           impl_->reassemblies.empty() && impl_->reliableReceiveBuffer.empty();
}
std::size_t DeterministicReplicationTransport::pending_reliable_messages() const noexcept {
    std::size_t count{};
    for (const auto& [link, messages] : impl_->pendingReliable) {
        (void)link;
        count += messages.size();
    }
    return count;
}
const ReplicationTransportTelemetry& DeterministicReplicationTransport::telemetry() const noexcept {
    return impl_->telemetry;
}
const SimulatedNetworkConfig& DeterministicReplicationTransport::config() const noexcept {
    return impl_->config;
}

ReplicationTransportCapabilities DeterministicReplicationTransport::capabilities() const noexcept {
    return {impl_->config.maximumMessageBytes, impl_->config.maximumDatagramBytes,
            true, true, true};
}

bool ReplicatedInputCommand::validate(std::string* error) const noexcept {
    if (pawn == kInvalidNetworkObjectId) {
        if (error != nullptr) *error = "replicated input has an invalid pawn id";
        return false;
    }
    if ((flags & ~0x03U) != 0U) {
        if (error != nullptr) *error = "replicated input contains unknown flags";
        return false;
    }
    return true;
}

ReplicatedInputCommand make_replicated_input_command(
    NetworkObjectId pawn, std::uint64_t clientTick, const CharacterInput& input) noexcept {
    Float3 movement = input.move;
    movement.z = 0.0F;
    if (length_squared(movement) > 1.0F) movement = normalize(movement);
    return {pawn, clientTick, quantize_input_axis(movement.x), quantize_input_axis(movement.y),
            static_cast<std::uint8_t>((input.jumpPressed ? 0x01U : 0U) |
                                      (input.crouchHeld ? 0x02U : 0U))};
}

CharacterInput decode_replicated_input(const ReplicatedInputCommand& command) noexcept {
    return {{static_cast<float>(command.moveX) / 32767.0F,
             static_cast<float>(command.moveY) / 32767.0F, 0.0F},
            (command.flags & 0x01U) != 0U, (command.flags & 0x02U) != 0U};
}

std::vector<std::uint8_t> encode_replicated_input_command(
    const ReplicatedInputCommand& command, std::string* error) {
    if (!command.validate(error)) return {};
    std::vector<std::uint8_t> output;
    output.reserve(32U);
    append_le(output, kInputMagic);
    append_le(output, kCodecVersion);
    append_le(output, static_cast<std::uint16_t>(0U));
    append_le(output, command.pawn);
    append_le(output, command.clientTick);
    append_le(output, command.moveX);
    append_le(output, command.moveY);
    output.push_back(command.flags);
    output.push_back(0U);
    return output;
}

std::optional<ReplicatedInputCommand> decode_replicated_input_command(
    std::span<const std::uint8_t> bytes, std::string* error) {
    const auto reject = [&](const char* message) -> std::optional<ReplicatedInputCommand> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    std::size_t cursor{};
    std::uint32_t magic{};
    std::uint16_t version{}, reserved{};
    ReplicatedInputCommand command;
    if (!read_le(bytes, cursor, magic) || !read_le(bytes, cursor, version) ||
        !read_le(bytes, cursor, reserved) || !read_le(bytes, cursor, command.pawn) ||
        !read_le(bytes, cursor, command.clientTick) || !read_le(bytes, cursor, command.moveX) ||
        !read_le(bytes, cursor, command.moveY) || cursor + 2U > bytes.size())
        return reject("replicated input command is truncated");
    command.flags = bytes[cursor++];
    const std::uint8_t reservedByte = bytes[cursor++];
    if (magic != kInputMagic || version != kCodecVersion || reserved != 0U || reservedByte != 0U)
        return reject("replicated input command header is unsupported");
    if (cursor != bytes.size()) return reject("replicated input command has trailing bytes");
    if (!command.validate(error)) return std::nullopt;
    return command;
}

bool ReplicatedBrickRepairRequest::validate(std::string* error) const noexcept {
    if (object == kInvalidNetworkObjectId || causingEditSequence == 0U) {
        if (error != nullptr) *error = "brick repair request has an invalid identity or sequence";
        return false;
    }
    if (expectedRevision == observedRevision && expectedContentHash == observedContentHash) {
        if (error != nullptr) *error = "brick repair request does not describe a divergence";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_brick_repair_request(
    const ReplicatedBrickRepairRequest& request, std::string* error) {
    if (!request.validate(error)) return {};
    std::vector<std::uint8_t> output;
    output.reserve(64U);
    append_le(output, kRepairMagic);
    append_le(output, kCodecVersion);
    append_le(output, static_cast<std::uint16_t>(0U));
    append_le(output, request.object);
    append_le(output, request.brick.x);
    append_le(output, request.brick.y);
    append_le(output, request.brick.z);
    append_le(output, request.causingEditSequence);
    append_le(output, request.expectedRevision);
    append_le(output, request.observedRevision);
    append_le(output, request.expectedContentHash);
    append_le(output, request.observedContentHash);
    return output;
}

std::optional<ReplicatedBrickRepairRequest> decode_brick_repair_request(
    std::span<const std::uint8_t> bytes, std::string* error) {
    const auto reject = [&](const char* message) -> std::optional<ReplicatedBrickRepairRequest> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    std::size_t cursor{};
    std::uint32_t magic{};
    std::uint16_t version{}, reserved{};
    ReplicatedBrickRepairRequest request;
    if (!read_le(bytes, cursor, magic) || !read_le(bytes, cursor, version) ||
        !read_le(bytes, cursor, reserved) || !read_le(bytes, cursor, request.object) ||
        !read_le(bytes, cursor, request.brick.x) || !read_le(bytes, cursor, request.brick.y) ||
        !read_le(bytes, cursor, request.brick.z) ||
        !read_le(bytes, cursor, request.causingEditSequence) ||
        !read_le(bytes, cursor, request.expectedRevision) ||
        !read_le(bytes, cursor, request.observedRevision) ||
        !read_le(bytes, cursor, request.expectedContentHash) ||
        !read_le(bytes, cursor, request.observedContentHash))
        return reject("brick repair request is truncated");
    if (magic != kRepairMagic || version != kCodecVersion || reserved != 0U)
        return reject("brick repair request header is unsupported");
    if (cursor != bytes.size()) return reject("brick repair request has trailing bytes");
    if (!request.validate(error)) return std::nullopt;
    return request;
}

bool ReplicatedBrickRepairResponse::validate(std::string* error) const noexcept {
    if (object == kInvalidNetworkObjectId || causingEditSequence == 0U) {
        if (error != nullptr) *error = "brick repair response has an invalid identity or sequence";
        return false;
    }
    if (VoxelObject::brick_content_hash(materials) != authoritativeContentHash) {
        if (error != nullptr) *error = "brick repair response content hash does not match its materials";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_brick_repair_response(
    const ReplicatedBrickRepairResponse& response, std::string* error) {
    if (!response.validate(error)) return {};
    std::vector<std::uint8_t> output;
    output.reserve(560U);
    append_le(output, kRepairResponseMagic);
    append_le(output, kCodecVersion);
    append_le(output, static_cast<std::uint16_t>(0U));
    append_le(output, response.object);
    append_le(output, response.brick.x);
    append_le(output, response.brick.y);
    append_le(output, response.brick.z);
    append_le(output, response.causingEditSequence);
    append_le(output, response.authoritativeRevision);
    append_le(output, response.authoritativeContentHash);
    output.insert(output.end(), response.materials.begin(), response.materials.end());
    return output;
}

std::optional<ReplicatedBrickRepairResponse> decode_brick_repair_response(
    std::span<const std::uint8_t> bytes, std::string* error) {
    const auto reject = [&](const char* message) -> std::optional<ReplicatedBrickRepairResponse> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    std::size_t cursor{};
    std::uint32_t magic{};
    std::uint16_t version{}, reserved{};
    ReplicatedBrickRepairResponse response;
    if (!read_le(bytes, cursor, magic) || !read_le(bytes, cursor, version) ||
        !read_le(bytes, cursor, reserved) || !read_le(bytes, cursor, response.object) ||
        !read_le(bytes, cursor, response.brick.x) || !read_le(bytes, cursor, response.brick.y) ||
        !read_le(bytes, cursor, response.brick.z) ||
        !read_le(bytes, cursor, response.causingEditSequence) ||
        !read_le(bytes, cursor, response.authoritativeRevision) ||
        !read_le(bytes, cursor, response.authoritativeContentHash))
        return reject("brick repair response is truncated");
    if (magic != kRepairResponseMagic || version != kCodecVersion || reserved != 0U)
        return reject("brick repair response header is unsupported");
    if (cursor + response.materials.size() != bytes.size())
        return reject("brick repair response has an invalid material payload size");
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end(),
              response.materials.begin());
    if (!response.validate(error)) return std::nullopt;
    return response;
}

std::optional<ReplicatedBrickRepairResponse> build_brick_repair_response(
    const GameWorld& authority, const ReplicationIdentityRegistry& identities,
    const ReplicatedBrickRepairRequest& request, std::string* error) {
    if (!request.validate(error)) return std::nullopt;
    const auto localObject = identities.local_object(request.object);
    if (!localObject) {
        if (error != nullptr) *error = "brick repair request references an unknown authority object";
        return std::nullopt;
    }
    const auto snapshot = authority.voxel_brick_snapshot(*localObject, BrickKey{request.brick.x, request.brick.y, request.brick.z});
    if (!snapshot) {
        if (error != nullptr) *error = "brick repair request target has no voxel brick state";
        return std::nullopt;
    }
    ReplicatedBrickRepairResponse response;
    response.object = request.object;
    response.brick = request.brick;
    response.causingEditSequence = request.causingEditSequence;
    response.authoritativeRevision = snapshot->revision;
    response.authoritativeContentHash = snapshot->contentHash;
    response.materials = snapshot->materials;
    return response;
}

bool apply_brick_repair_response(
    GameWorld& replica, const ReplicationIdentityRegistry& identities,
    const ReplicatedBrickRepairResponse& response, std::string* error) {
    if (!response.validate(error)) return false;
    const auto localObject = identities.local_object(response.object);
    if (!localObject) {
        if (error != nullptr) *error = "brick repair response references an unknown replica object";
        return false;
    }
    GameVoxelBrickSnapshot snapshot;
    snapshot.brick = BrickKey{response.brick.x, response.brick.y, response.brick.z};
    snapshot.revision = response.authoritativeRevision;
    snapshot.materials = response.materials;
    snapshot.contentHash = response.authoritativeContentHash;
    if (!replica.replace_voxel_brick(*localObject, snapshot, error)) return false;
    const auto verified = replica.voxel_brick_snapshot(*localObject, BrickKey{response.brick.x, response.brick.y, response.brick.z});
    if (!verified || verified->revision != response.authoritativeRevision ||
        verified->contentHash != response.authoritativeContentHash) {
        if (error != nullptr) *error = "brick repair application did not reproduce authority state";
        return false;
    }
    return true;
}

DestructionReplicaResult DestructionReplicaTracker::accept(
    const ReplicatedDestructionEdit& edit, std::uint32_t observedRevision,
    std::uint64_t observedContentHash, ReplicatedBrickRepairRequest* repairRequest,
    std::string* error) noexcept {
    if (edit.object == kInvalidNetworkObjectId || edit.editSequence == 0U ||
        edit.radiusMillimeters == 0U) {
        if (error != nullptr) *error = "destruction replica received an invalid edit";
        return DestructionReplicaResult::Invalid;
    }
    if (edit.editSequence <= lastAppliedSequence_) return DestructionReplicaResult::Duplicate;
    if (edit.editSequence != lastAppliedSequence_ + 1U) {
        if (error != nullptr) *error = "destruction replica detected a sequence gap";
        return DestructionReplicaResult::SequenceGap;
    }
    lastAppliedSequence_ = edit.editSequence;
    if (observedRevision != edit.expectedBrickRevision ||
        observedContentHash != edit.expectedContentHash) {
        if (repairRequest != nullptr) {
            *repairRequest = {edit.object, edit.brick, edit.editSequence,
                              edit.expectedBrickRevision, observedRevision,
                              edit.expectedContentHash, observedContentHash};
        }
        return DestructionReplicaResult::RepairRequired;
    }
    return DestructionReplicaResult::Applied;
}

} // namespace dve

namespace dve {

bool ClientPredictionConfig::validate(std::string* error) const noexcept {
    const auto reject = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!std::isfinite(positionToleranceMeters) || positionToleranceMeters < 0.0F)
        return reject("prediction position tolerance must be finite and non-negative");
    if (!std::isfinite(velocityToleranceMetersPerSecond) || velocityToleranceMetersPerSecond < 0.0F)
        return reject("prediction velocity tolerance must be finite and non-negative");
    if (!std::isfinite(hardSnapDistanceMeters) ||
        hardSnapDistanceMeters < positionToleranceMeters)
        return reject("prediction hard-snap distance must be finite and at least the position tolerance");
    if (maximumHistory == 0U || maximumHistory > 65536U)
        return reject("prediction history limit is outside the supported range");
    return true;
}

ClientPredictionBuffer::ClientPredictionBuffer(ClientPredictionConfig config)
    : config_(std::move(config)) {
    if (!config_.validate()) config_ = {};
    records_.reserve(config_.maximumHistory);
}

bool ClientPredictionBuffer::push(
    const ReplicatedInputCommand& input, const ReplicatedCharacterState& predicted,
    std::string* error) {
    if (!input.validate(error)) return false;
    if (predicted.pawn == kInvalidNetworkObjectId || predicted.pawn != input.pawn) {
        if (error != nullptr) *error = "predicted character identity must match the input pawn";
        return false;
    }
    if (!records_.empty() && input.clientTick <= records_.back().input.clientTick) {
        if (error != nullptr) *error = "prediction ticks must increase strictly";
        return false;
    }
    if (records_.size() == config_.maximumHistory) records_.erase(records_.begin());
    records_.push_back({input, predicted});
    return true;
}

std::optional<ClientReconciliationResult> ClientPredictionBuffer::reconcile(
    std::uint64_t authoritativeTick, const ReplicatedCharacterState& authoritative,
    std::string* error) {
    if (authoritative.pawn == kInvalidNetworkObjectId) {
        if (error != nullptr) *error = "authoritative character identity is invalid";
        return std::nullopt;
    }
    ClientReconciliationResult result;
    result.authoritativeTick = authoritativeTick;
    result.authoritativePosition = dequantize_position_meters(authoritative.position);
    result.authoritativeVelocity = dequantize_velocity_meters(authoritative.velocity);

    auto matching = std::find_if(records_.begin(), records_.end(), [&](const ClientPredictionRecord& record) {
        return record.input.clientTick == authoritativeTick && record.predicted.pawn == authoritative.pawn;
    });
    if (matching != records_.end()) {
        result.matchingPredictionFound = true;
        const Float3 predictedPosition = dequantize_position_meters(matching->predicted.position);
        const Float3 predictedVelocity = dequantize_velocity_meters(matching->predicted.velocity);
        result.positionErrorMeters = length(subtract(predictedPosition, result.authoritativePosition));
        result.velocityErrorMetersPerSecond = length(subtract(predictedVelocity, result.authoritativeVelocity));
        result.correctionRequired = result.positionErrorMeters > config_.positionToleranceMeters ||
            result.velocityErrorMetersPerSecond > config_.velocityToleranceMetersPerSecond;
        result.hardSnap = result.positionErrorMeters > config_.hardSnapDistanceMeters;
    } else {
        result.correctionRequired = true;
        result.hardSnap = true;
    }

    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const ClientPredictionRecord& record) {
        return record.input.clientTick <= authoritativeTick;
    }), records_.end());
    result.inputsToReplay.reserve(records_.size());
    for (const ClientPredictionRecord& record : records_) {
        if (record.predicted.pawn == authoritative.pawn)
            result.inputsToReplay.push_back(record.input);
    }
    return result;
}

ClientGameplayPrediction::ClientGameplayPrediction(
    GameWorld& world, ReplicationIdentityRegistry& identities,
    GameObjectId localPawn, float fixedDeltaSeconds, ClientPredictionConfig config)
    : world_(&world), identities_(&identities), localPawn_(localPawn),
      fixedDeltaSeconds_(fixedDeltaSeconds), predictions_(std::move(config)) {
    if (!(fixedDeltaSeconds_ > 0.0F) || !std::isfinite(fixedDeltaSeconds_))
        fixedDeltaSeconds_ = 1.0F / 60.0F;
    if (world_->gameplay().has_character(localPawn_))
        networkPawn_ = identities_->ensure_object(localPawn_);
}

std::optional<ReplicatedCharacterState> ClientGameplayPrediction::capture(
    std::string* error) const {
    if (networkPawn_ == kInvalidNetworkObjectId || !world_->gameplay().has_character(localPawn_)) {
        if (error != nullptr) *error = "prediction controller has no bound live character";
        return std::nullopt;
    }
    const auto position = world_->position(localPawn_);
    const auto* state = world_->gameplay().character(localPawn_);
    if (!position || state == nullptr) {
        if (error != nullptr) *error = "prediction character state is unavailable";
        return std::nullopt;
    }
    ReplicatedCharacterState replicated;
    replicated.pawn = networkPawn_;
    replicated.position = quantize_position_millimeters(*position);
    replicated.velocity = quantize_velocity_centimeters(state->velocity);
    replicated.flags = static_cast<std::uint8_t>((state->grounded ? 0x01U : 0U) |
        (state->stance == CharacterStance::Crouched ? 0x02U : 0U));
    if (state->supportObject != kInvalidGameObjectId)
        replicated.support = identities_->ensure_object(state->supportObject);
    return replicated;
}

bool ClientGameplayPrediction::predict(
    std::uint64_t clientTick, const CharacterInput& input, std::string* error) {
    if (networkPawn_ == kInvalidNetworkObjectId || clientTick == 0U) {
        if (error != nullptr) *error = "prediction controller or client tick is invalid";
        return false;
    }
    const ReplicatedInputCommand command =
        make_replicated_input_command(networkPawn_, clientTick, input);
    if (!world_->gameplay().simulate_character_input(
            localPawn_, decode_replicated_input(command), fixedDeltaSeconds_, clientTick, error))
        return false;
    const auto predicted = capture(error);
    return predicted && predictions_.push(command, *predicted, error);
}

std::optional<ClientPredictionExecutionResult> ClientGameplayPrediction::reconcile(
    std::uint64_t authoritativeTick, const ReplicatedCharacterState& authoritative,
    std::string* error) {
    if (authoritative.pawn != networkPawn_) {
        if (error != nullptr) *error = "authoritative state does not target this prediction controller";
        return std::nullopt;
    }
    const auto analysis = predictions_.reconcile(authoritativeTick, authoritative, error);
    if (!analysis) return std::nullopt;
    ClientPredictionExecutionResult execution;
    execution.reconciliation = *analysis;

    if (analysis->correctionRequired) {
        GameObjectId support = kInvalidGameObjectId;
        if (authoritative.support != kInvalidNetworkObjectId) {
            if (const auto localSupport = identities_->local_object(authoritative.support))
                support = *localSupport;
        }
        const bool grounded = (authoritative.flags & 0x01U) != 0U;
        const CharacterStance stance = (authoritative.flags & 0x02U) != 0U
            ? CharacterStance::Crouched : CharacterStance::Standing;
        if (!world_->gameplay().apply_authoritative_character_state(
                localPawn_, analysis->authoritativePosition, analysis->authoritativeVelocity,
                grounded, support, stance, authoritativeTick, error))
            return std::nullopt;
        execution.authoritativeStateApplied = true;
        predictions_.clear();
        for (const ReplicatedInputCommand& command : analysis->inputsToReplay) {
            if (!world_->gameplay().simulate_character_input(
                    localPawn_, decode_replicated_input(command), fixedDeltaSeconds_,
                    command.clientTick, error))
                return std::nullopt;
            const auto replayed = capture(error);
            if (!replayed || !predictions_.push(command, *replayed, error)) return std::nullopt;
            ++execution.replayedInputs;
        }
    }

    const auto position = world_->position(localPawn_);
    const auto* state = world_->gameplay().character(localPawn_);
    if (!position || state == nullptr) {
        if (error != nullptr) *error = "reconciled character state is unavailable";
        return std::nullopt;
    }
    execution.finalPosition = *position;
    execution.finalVelocity = state->velocity;
    return execution;
}

} // namespace dve
