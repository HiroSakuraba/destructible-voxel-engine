#include "dve/network_udp.hpp"

#include "dve/network_crypto.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dve {
namespace {

constexpr std::uint32_t kUdpMagic = 0x55564544U; // DVEU little endian
constexpr std::uint16_t kUdpVersion = 1U;
constexpr std::size_t kTagBytes = 16U;
constexpr std::size_t kHeaderBytes = 42U;
constexpr std::size_t kMinimumKeyBytes = 16U;
constexpr std::uint16_t kMaximumFragments = 4096U;

enum class PacketType : std::uint8_t { Hello = 1, Challenge = 2, Proof = 3, Accept = 4, Data = 5, Ack = 6 };
enum class HandshakeState : std::uint8_t { Idle, HelloSent, ChallengeSent, ProofSent, Authenticated, Failed };

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle socket) noexcept { if (socket != kInvalidSocket) closesocket(socket); }
int socket_error() noexcept { return WSAGetLastError(); }
bool would_block(int error) noexcept { return error == WSAEWOULDBLOCK; }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle socket) noexcept { if (socket != kInvalidSocket) ::close(socket); }
int socket_error() noexcept { return errno; }
bool would_block(int error) noexcept { return error == EAGAIN || error == EWOULDBLOCK; }
#endif

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

struct EndpointAddress {
    sockaddr_storage storage{};
    socklen_t length{};
    bool known{};
};

bool resolve_ipv4(std::string_view address, std::uint16_t port, EndpointAddress& output) noexcept {
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string(address).c_str(), &ipv4.sin_addr) != 1) return false;
    std::memcpy(&output.storage, &ipv4, sizeof(ipv4));
    output.length = sizeof(ipv4);
    output.known = port != 0U;
    return true;
}

std::uint16_t endpoint_port(const EndpointAddress& endpoint) noexcept {
    if (!endpoint.known || endpoint.storage.ss_family != AF_INET) return 0U;
    const auto* address = reinterpret_cast<const sockaddr_in*>(&endpoint.storage);
    return ntohs(address->sin_port);
}

bool same_endpoint(const EndpointAddress& expected, const sockaddr_storage& actual, socklen_t actualLength) noexcept {
    if (!expected.known || expected.length != actualLength || expected.storage.ss_family != actual.ss_family)
        return false;
    if (actual.ss_family == AF_INET) {
        const auto* lhs = reinterpret_cast<const sockaddr_in*>(&expected.storage);
        const auto* rhs = reinterpret_cast<const sockaddr_in*>(&actual);
        return lhs->sin_port == rhs->sin_port && lhs->sin_addr.s_addr == rhs->sin_addr.s_addr;
    }
    return false;
}

EndpointAddress from_sockaddr(const sockaddr_storage& address, socklen_t length) noexcept {
    EndpointAddress output;
    output.storage = address;
    output.length = length;
    output.known = true;
    return output;
}

struct PacketHeader {
    PacketType type{PacketType::Data};
    ReplicationTransportChannel channel{ReplicationTransportChannel::ReliableOrdered};
    ReplicationMessageKind kind{ReplicationMessageKind::UserPayload};
    ReplicationPeerId source{};
    ReplicationPeerId destination{};
    std::uint64_t sessionId{};
    std::uint64_t sequence{};
    std::uint16_t fragmentIndex{};
    std::uint16_t fragmentCount{1U};
    std::uint16_t payloadBytes{};
};

std::vector<std::uint8_t> encode_header(const PacketHeader& header) {
    std::vector<std::uint8_t> output;
    output.reserve(kHeaderBytes);
    append_le(output, kUdpMagic);
    append_le(output, kUdpVersion);
    output.push_back(static_cast<std::uint8_t>(header.type));
    output.push_back(static_cast<std::uint8_t>(header.channel));
    output.push_back(static_cast<std::uint8_t>(header.kind));
    output.push_back(0U);
    append_le(output, header.source);
    append_le(output, header.destination);
    append_le(output, header.sessionId);
    append_le(output, header.sequence);
    append_le(output, header.fragmentIndex);
    append_le(output, header.fragmentCount);
    append_le(output, header.payloadBytes);
    append_le(output, static_cast<std::uint16_t>(0U));
    return output;
}

bool decode_header(std::span<const std::uint8_t> bytes, PacketHeader& header) noexcept {
    if (bytes.size() < kHeaderBytes + kTagBytes) return false;
    std::size_t cursor{};
    std::uint32_t magic{};
    std::uint16_t version{}, reserved{};
    std::uint8_t type{}, channel{}, kind{}, reservedByte{};
    if (!read_le(bytes, cursor, magic) || !read_le(bytes, cursor, version) ||
        !read_le(bytes, cursor, type) || !read_le(bytes, cursor, channel) ||
        !read_le(bytes, cursor, kind) || !read_le(bytes, cursor, reservedByte) ||
        !read_le(bytes, cursor, header.source) || !read_le(bytes, cursor, header.destination) ||
        !read_le(bytes, cursor, header.sessionId) || !read_le(bytes, cursor, header.sequence) ||
        !read_le(bytes, cursor, header.fragmentIndex) || !read_le(bytes, cursor, header.fragmentCount) ||
        !read_le(bytes, cursor, header.payloadBytes) || !read_le(bytes, cursor, reserved)) return false;
    if (magic != kUdpMagic || version != kUdpVersion || reservedByte != 0U || reserved != 0U ||
        type < static_cast<std::uint8_t>(PacketType::Hello) || type > static_cast<std::uint8_t>(PacketType::Ack) ||
        channel > static_cast<std::uint8_t>(ReplicationTransportChannel::UnreliableSequenced) ||
        kind > static_cast<std::uint8_t>(ReplicationMessageKind::UserPayload) ||
        header.fragmentCount == 0U || header.fragmentIndex >= header.fragmentCount ||
        header.fragmentCount > kMaximumFragments ||
        kHeaderBytes + static_cast<std::size_t>(header.payloadBytes) + kTagBytes != bytes.size()) return false;
    header.type = static_cast<PacketType>(type);
    header.channel = static_cast<ReplicationTransportChannel>(channel);
    header.kind = static_cast<ReplicationMessageKind>(kind);
    return true;
}

std::vector<std::uint8_t> authenticated_packet(
    const PacketHeader& header, std::span<const std::uint8_t> payload,
    std::span<const std::uint8_t> key) {
    std::vector<std::uint8_t> output = encode_header(header);
    output.insert(output.end(), payload.begin(), payload.end());
    const auto digest = network_hmac_sha256(key, output);
    output.insert(output.end(), digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(kTagBytes));
    return output;
}

bool verify_packet(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> key) noexcept {
    if (bytes.size() < kHeaderBytes + kTagBytes) return false;
    const auto signedBytes = bytes.first(bytes.size() - kTagBytes);
    const auto supplied = bytes.last(kTagBytes);
    const auto digest = network_hmac_sha256(key, signedBytes);
    return network_constant_time_equal(supplied,
        std::span<const std::uint8_t>(digest.data(), kTagBytes));
}

std::vector<std::uint8_t> session_material(
    std::uint64_t clientNonce, std::uint64_t serverNonce, std::uint64_t sessionId,
    ReplicationPeerId clientPeer, ReplicationPeerId serverPeer) {
    std::vector<std::uint8_t> material;
    static constexpr std::array<std::uint8_t, 11> label{'D','V','E','-','S','E','S','S','I','O','N'};
    material.insert(material.end(), label.begin(), label.end());
    append_le(material, clientNonce); append_le(material, serverNonce); append_le(material, sessionId);
    append_le(material, clientPeer); append_le(material, serverPeer);
    return material;
}

struct FragmentAssembly {
    ReplicationTransportChannel channel{ReplicationTransportChannel::ReliableOrdered};
    ReplicationMessageKind kind{ReplicationMessageKind::UserPayload};
    std::uint64_t sequence{};
    std::uint64_t firstTick{};
    std::vector<std::vector<std::uint8_t>> fragments;
    std::vector<bool> received;
    std::size_t bytes{};
};

struct PendingReliable {
    ReplicationMessageKind kind{ReplicationMessageKind::UserPayload};
    std::uint64_t sequence{};
    std::vector<std::vector<std::uint8_t>> datagrams;
    std::vector<bool> acknowledged;
    std::uint64_t lastSendTick{};
    std::uint32_t attempts{};
};

struct PeerState {
    UdpReplicationPeerConfig config;
    EndpointAddress endpoint;
    HandshakeState handshake{HandshakeState::Idle};
    std::uint64_t handshakeStartTick{};
    std::uint64_t lastHandshakeTick{};
    std::uint32_t handshakeAttempts{};
    std::uint64_t clientNonce{};
    std::uint64_t serverNonce{};
    std::uint64_t sessionId{};
    NetworkSha256Digest sessionKey{};
    std::uint64_t authenticatedTick{};
    std::deque<std::uint64_t> recentClientNonces;
    std::uint64_t nextReliableSequence{1U};
    std::uint64_t nextUnreliableSequence{1U};
    std::uint64_t expectedReliableSequence{1U};
    std::uint64_t latestUnreliableSequence{};
    std::map<std::uint64_t, PendingReliable> pendingReliable;
    std::map<std::uint64_t, FragmentAssembly> reliableAssemblies;
    std::map<std::uint64_t, FragmentAssembly> unreliableAssemblies;
    std::map<std::uint64_t, ReplicationTransportMessage> completedReliable;
};

bool all_true(const std::vector<bool>& values) {
    return std::all_of(values.begin(), values.end(), [](bool value) { return value; });
}

} // namespace

bool UdpReplicationTransportConfig::validate(std::string* error) const noexcept {
    if (localPeer == kInvalidReplicationPeerId) {
        if (error) *error = "UDP transport requires a nonzero local peer id";
        return false;
    }
    if (bindAddress.empty() || maximumDatagramBytes < kHeaderBytes + kTagBytes + 32U ||
        maximumDatagramBytes > 65507U || maximumMessageBytes == 0U ||
        maximumMessageBytes > 64U * 1024U * 1024U || maximumPendingReliableMessages == 0U ||
        maximumReassemblies == 0U || reliableRetryTicks == 0U || maximumReliableAttempts == 0U ||
        handshakeRetryTicks == 0U || handshakeTimeoutTicks < handshakeRetryTicks ||
        reassemblyTimeoutTicks == 0U) {
        if (error) *error = "UDP transport limits are invalid";
        return false;
    }
    return true;
}

bool UdpReplicationPeerConfig::validate(std::string* error) const noexcept {
    if (peer == kInvalidReplicationPeerId || address.empty() ||
        (!allowAuthenticatedPortLearning && port == 0U) || preSharedKey.size() < kMinimumKeyBytes ||
        preSharedKey.size() > 1024U) {
        if (error) *error = "UDP peer id, endpoint, or pre-shared key is invalid";
        return false;
    }
    return true;
}

struct UdpReplicationTransport::Impl {
    explicit Impl(UdpReplicationTransportConfig value) : config(std::move(value)) {}
    ~Impl() {
        for (auto& [_, peer] : peers) {
            std::fill(peer.config.preSharedKey.begin(), peer.config.preSharedKey.end(), 0U);
            peer.sessionKey.fill(0U);
        }
        close_socket(socket);
#if defined(_WIN32)
        if (winsockInitialized) WSACleanup();
#endif
    }

    UdpReplicationTransportConfig config;
    SocketHandle socket{kInvalidSocket};
    bool valid{};
    std::string error;
    std::uint16_t localPort{};
    std::uint64_t currentTick{};
    std::uint64_t nonceState{};
    ReplicationTransportTelemetry telemetry{};
    std::unordered_map<ReplicationPeerId, PeerState> peers;
    std::unordered_map<ReplicationPeerId, std::deque<ReplicationTransportMessage>> delivered;
#if defined(_WIN32)
    bool winsockInitialized{};
#endif

    std::uint64_t random_u64() {
        if (nonceState == 0U) {
            std::random_device device;
            nonceState = (static_cast<std::uint64_t>(device()) << 32U) ^ device() ^
                static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            if (nonceState == 0U) nonceState = 0xD6E148A5BADC0FFEULL;
        }
        nonceState ^= nonceState >> 12U;
        nonceState ^= nonceState << 25U;
        nonceState ^= nonceState >> 27U;
        const std::uint64_t result = nonceState * 2685821657736338717ULL;
        return result == 0U ? 1U : result;
    }

    bool transmit(const EndpointAddress& endpoint, std::span<const std::uint8_t> bytes) {
        if (!endpoint.known || socket == kInvalidSocket) return false;
        ++telemetry.datagramsAttempted;
        telemetry.bytesAttempted += bytes.size();
#if defined(_WIN32)
        const int sent = ::sendto(socket, reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&endpoint.storage), endpoint.length);
        const bool success = sent == static_cast<int>(bytes.size());
#else
        const ssize_t sent = ::sendto(socket, bytes.data(), bytes.size(), 0,
            reinterpret_cast<const sockaddr*>(&endpoint.storage), endpoint.length);
        const bool success = sent == static_cast<ssize_t>(bytes.size());
#endif
        if (success) ++telemetry.datagramsScheduled;
        return success;
    }

    NetworkSha256Digest derive_session_key(const PeerState& peer) const {
        const bool localIsClient = peer.config.initiateHandshake;
        const ReplicationPeerId clientPeer = localIsClient ? config.localPeer : peer.config.peer;
        const ReplicationPeerId serverPeer = localIsClient ? peer.config.peer : config.localPeer;
        const auto material = session_material(peer.clientNonce, peer.serverNonce, peer.sessionId,
                                                clientPeer, serverPeer);
        return network_hmac_sha256(peer.config.preSharedKey, material);
    }

    bool send_handshake(PeerState& peer, PacketType type) {
        PacketHeader header;
        header.type = type;
        header.source = config.localPeer;
        header.destination = peer.config.peer;
        header.sessionId = (type == PacketType::Hello) ? 0U : peer.sessionId;
        std::vector<std::uint8_t> payload;
        if (type == PacketType::Hello) {
            append_le(payload, peer.clientNonce);
        } else if (type == PacketType::Challenge || type == PacketType::Proof) {
            append_le(payload, peer.clientNonce);
            append_le(payload, peer.serverNonce);
        } else if (type == PacketType::Accept) {
            append_le(payload, peer.serverNonce);
        }
        header.payloadBytes = static_cast<std::uint16_t>(payload.size());
        const auto key = (type == PacketType::Proof || type == PacketType::Accept)
            ? std::span<const std::uint8_t>(peer.sessionKey)
            : std::span<const std::uint8_t>(peer.config.preSharedKey);
        const auto datagram = authenticated_packet(header, payload, key);
        if (!transmit(peer.endpoint, datagram)) return false;
        peer.lastHandshakeTick = currentTick;
        ++peer.handshakeAttempts;
        return true;
    }

    void fail_handshake(PeerState& peer) {
        if (peer.handshake != HandshakeState::Failed) ++telemetry.handshakesFailed;
        peer.handshake = HandshakeState::Failed;
        peer.sessionKey.fill(0U);
        peer.sessionId = 0U;
    }

    bool endpoint_allowed(PeerState& peer, const sockaddr_storage& from, socklen_t fromLength,
                          bool authenticatedHandshakePacket) {
        if (same_endpoint(peer.endpoint, from, fromLength)) return true;
        if (peer.config.allowAuthenticatedPortLearning && authenticatedHandshakePacket) {
            EndpointAddress candidate = from_sockaddr(from, fromLength);
            if (candidate.storage.ss_family != AF_INET) return false;
            const auto* expected = reinterpret_cast<const sockaddr_in*>(&peer.endpoint.storage);
            const auto* actual = reinterpret_cast<const sockaddr_in*>(&candidate.storage);
            if (expected->sin_addr.s_addr != actual->sin_addr.s_addr) return false;
            peer.endpoint = candidate;
            peer.config.port = endpoint_port(candidate);
            return true;
        }
        return false;
    }

    void send_ack(PeerState& peer, const PacketHeader& received) {
        PacketHeader ack;
        ack.type = PacketType::Ack;
        ack.channel = ReplicationTransportChannel::ReliableOrdered;
        ack.kind = received.kind;
        ack.source = config.localPeer;
        ack.destination = peer.config.peer;
        ack.sessionId = peer.sessionId;
        ack.sequence = received.sequence;
        ack.fragmentIndex = received.fragmentIndex;
        ack.fragmentCount = received.fragmentCount;
        const auto datagram = authenticated_packet(ack, {}, peer.sessionKey);
        (void)transmit(peer.endpoint, datagram);
    }

    void complete_assembly(PeerState& peer, FragmentAssembly& assembly) {
        ReplicationTransportMessage message;
        message.source = peer.config.peer;
        message.destination = config.localPeer;
        message.channel = assembly.channel;
        message.kind = assembly.kind;
        message.sequence = assembly.sequence;
        message.deliveryTick = currentTick;
        message.payload.reserve(assembly.bytes);
        for (const auto& fragment : assembly.fragments)
            message.payload.insert(message.payload.end(), fragment.begin(), fragment.end());
        if (assembly.channel == ReplicationTransportChannel::ReliableOrdered) {
            peer.completedReliable.emplace(message.sequence, std::move(message));
            for (;;) {
                const auto found = peer.completedReliable.find(peer.expectedReliableSequence);
                if (found == peer.completedReliable.end()) break;
                delivered[config.localPeer].push_back(std::move(found->second));
                peer.completedReliable.erase(found);
                ++peer.expectedReliableSequence;
                ++telemetry.logicalMessagesDelivered;
            }
        } else if (message.sequence > peer.latestUnreliableSequence) {
            peer.latestUnreliableSequence = message.sequence;
            delivered[config.localPeer].push_back(std::move(message));
            ++telemetry.logicalMessagesDelivered;
        } else {
            ++telemetry.staleUnreliableDatagrams;
        }
    }

    void process_data(PeerState& peer, const PacketHeader& header,
                      std::span<const std::uint8_t> payload) {
        if (header.channel == ReplicationTransportChannel::ReliableOrdered) send_ack(peer, header);
        auto& assemblies = header.channel == ReplicationTransportChannel::ReliableOrdered
            ? peer.reliableAssemblies : peer.unreliableAssemblies;
        if (!assemblies.contains(header.sequence) && assemblies.size() >= config.maximumReassemblies) {
            ++telemetry.datagramsRejected;
            return;
        }
        auto [iterator, inserted] = assemblies.try_emplace(header.sequence);
        FragmentAssembly& assembly = iterator->second;
        if (inserted) {
            assembly.channel = header.channel;
            assembly.kind = header.kind;
            assembly.sequence = header.sequence;
            assembly.firstTick = currentTick;
            assembly.fragments.resize(header.fragmentCount);
            assembly.received.assign(header.fragmentCount, false);
            telemetry.peakReassemblies = std::max<std::uint64_t>(telemetry.peakReassemblies,
                peer.reliableAssemblies.size() + peer.unreliableAssemblies.size());
        }
        if (assembly.fragments.size() != header.fragmentCount || assembly.kind != header.kind ||
            assembly.channel != header.channel) {
            ++telemetry.datagramsRejected;
            assemblies.erase(iterator);
            return;
        }
        if (!assembly.received[header.fragmentIndex]) {
            if (assembly.bytes + payload.size() > config.maximumMessageBytes) {
                ++telemetry.datagramsRejected;
                assemblies.erase(iterator);
                return;
            }
            assembly.fragments[header.fragmentIndex].assign(payload.begin(), payload.end());
            assembly.received[header.fragmentIndex] = true;
            assembly.bytes += payload.size();
        }
        if (all_true(assembly.received)) {
            complete_assembly(peer, assembly);
            assemblies.erase(header.sequence);
        }
    }

    void process_datagram(std::span<const std::uint8_t> bytes,
                          const sockaddr_storage& from, socklen_t fromLength) {
        PacketHeader header;
        if (!decode_header(bytes, header) || header.destination != config.localPeer ||
            header.source == kInvalidReplicationPeerId || header.source == config.localPeer) {
            ++telemetry.datagramsRejected;
            return;
        }
        const auto peerIterator = peers.find(header.source);
        if (peerIterator == peers.end()) {
            ++telemetry.datagramsRejected;
            return;
        }
        PeerState& peer = peerIterator->second;
        const auto payload = bytes.subspan(kHeaderBytes, header.payloadBytes);
        const bool handshake = header.type == PacketType::Hello || header.type == PacketType::Challenge ||
                               header.type == PacketType::Proof || header.type == PacketType::Accept;
        const auto authKey = (header.type == PacketType::Proof || header.type == PacketType::Accept ||
                              header.type == PacketType::Data || header.type == PacketType::Ack)
            ? std::span<const std::uint8_t>(peer.sessionKey)
            : std::span<const std::uint8_t>(peer.config.preSharedKey);
        if ((header.type == PacketType::Data || header.type == PacketType::Ack ||
             header.type == PacketType::Proof || header.type == PacketType::Accept) &&
            peer.sessionId == 0U) {
            ++telemetry.authenticationFailures;
            return;
        }
        if (!verify_packet(bytes, authKey)) {
            ++telemetry.authenticationFailures;
            return;
        }
        if (!endpoint_allowed(peer, from, fromLength, handshake)) {
            ++telemetry.authenticationFailures;
            return;
        }
        ++telemetry.datagramsDelivered;
        telemetry.bytesDelivered += bytes.size();

        std::size_t cursor{};
        if (header.type == PacketType::Hello) {
            std::uint64_t clientNonce{};
            if (payload.size() != sizeof(clientNonce) || !read_le(payload, cursor, clientNonce) ||
                peer.config.initiateHandshake) {
                ++telemetry.datagramsRejected;
                return;
            }
            if (std::find(peer.recentClientNonces.begin(), peer.recentClientNonces.end(), clientNonce) !=
                peer.recentClientNonces.end()) {
                if (clientNonce == peer.clientNonce &&
                    (peer.handshake == HandshakeState::ChallengeSent ||
                     peer.handshake == HandshakeState::Authenticated))
                    (void)send_handshake(peer, PacketType::Challenge);
                else
                    ++telemetry.authenticationFailures;
                return;
            }
            peer.recentClientNonces.push_back(clientNonce);
            while (peer.recentClientNonces.size() > 8U) peer.recentClientNonces.pop_front();
            peer.clientNonce = clientNonce;
            peer.serverNonce = random_u64();
            peer.sessionId = random_u64();
            peer.sessionKey = derive_session_key(peer);
            peer.handshake = HandshakeState::ChallengeSent;
            peer.handshakeStartTick = currentTick;
            (void)send_handshake(peer, PacketType::Challenge);
            return;
        }
        if (header.type == PacketType::Challenge) {
            std::uint64_t clientNonce{}, serverNonce{};
            if (payload.size() != 16U || !read_le(payload, cursor, clientNonce) ||
                !read_le(payload, cursor, serverNonce) || !peer.config.initiateHandshake ||
                clientNonce != peer.clientNonce || header.sessionId == 0U) {
                ++telemetry.datagramsRejected;
                return;
            }
            if (peer.handshake == HandshakeState::Authenticated &&
                peer.serverNonce == serverNonce && peer.sessionId == header.sessionId) {
                (void)send_handshake(peer, PacketType::Proof);
                return;
            }
            peer.serverNonce = serverNonce;
            peer.sessionId = header.sessionId;
            peer.sessionKey = derive_session_key(peer);
            peer.handshake = HandshakeState::ProofSent;
            (void)send_handshake(peer, PacketType::Proof);
            return;
        }
        if (header.type == PacketType::Proof) {
            std::uint64_t clientNonce{}, serverNonce{};
            if (payload.size() != 16U || !read_le(payload, cursor, clientNonce) ||
                !read_le(payload, cursor, serverNonce) || peer.config.initiateHandshake ||
                header.sessionId != peer.sessionId || clientNonce != peer.clientNonce ||
                serverNonce != peer.serverNonce) {
                ++telemetry.datagramsRejected;
                return;
            }
            if (peer.handshake != HandshakeState::Authenticated) {
                peer.handshake = HandshakeState::Authenticated;
                peer.authenticatedTick = currentTick;
                ++telemetry.handshakesCompleted;
            }
            (void)send_handshake(peer, PacketType::Accept);
            return;
        }
        if (header.type == PacketType::Accept) {
            std::uint64_t serverNonce{};
            if (payload.size() != sizeof(serverNonce) || !read_le(payload, cursor, serverNonce) ||
                !peer.config.initiateHandshake || header.sessionId != peer.sessionId ||
                serverNonce != peer.serverNonce) {
                ++telemetry.datagramsRejected;
                return;
            }
            if (peer.handshake != HandshakeState::Authenticated) {
                peer.handshake = HandshakeState::Authenticated;
                peer.authenticatedTick = currentTick;
                ++telemetry.handshakesCompleted;
            }
            return;
        }
        if (peer.handshake != HandshakeState::Authenticated || header.sessionId != peer.sessionId) {
            ++telemetry.authenticationFailures;
            return;
        }
        if (header.type == PacketType::Ack) {
            const auto pending = peer.pendingReliable.find(header.sequence);
            if (pending == peer.pendingReliable.end() ||
                header.fragmentIndex >= pending->second.acknowledged.size()) return;
            pending->second.acknowledged[header.fragmentIndex] = true;
            if (all_true(pending->second.acknowledged)) {
                peer.pendingReliable.erase(pending);
                ++telemetry.reliableMessagesAcknowledged;
            }
            return;
        }
        if (header.type == PacketType::Data) process_data(peer, header, payload);
    }

    void poll_socket() {
        std::array<std::uint8_t, 65536> buffer{};
        for (;;) {
            sockaddr_storage from{};
            socklen_t fromLength = sizeof(from);
#if defined(_WIN32)
            const int received = ::recvfrom(socket, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received == SOCKET_ERROR) {
                const int errorCode = socket_error();
                if (would_block(errorCode)) break;
                error = "UDP receive failed: " + std::to_string(errorCode);
                break;
            }
            if (received == 0) break;
            process_datagram(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
                             from, fromLength);
#else
            const ssize_t received = ::recvfrom(socket, buffer.data(), buffer.size(), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received < 0) {
                const int errorCode = socket_error();
                if (would_block(errorCode)) break;
                error = "UDP receive failed: " + std::to_string(errorCode);
                break;
            }
            if (received == 0) break;
            process_datagram(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
                             from, fromLength);
#endif
        }
    }

    void service_handshakes() {
        for (auto& [_, peer] : peers) {
            if (peer.handshake == HandshakeState::Authenticated || peer.handshake == HandshakeState::Idle ||
                peer.handshake == HandshakeState::Failed) continue;
            if (currentTick - peer.handshakeStartTick > config.handshakeTimeoutTicks) {
                fail_handshake(peer);
                continue;
            }
            if (currentTick - peer.lastHandshakeTick < config.handshakeRetryTicks) continue;
            if (peer.handshake == HandshakeState::HelloSent) (void)send_handshake(peer, PacketType::Hello);
            else if (peer.handshake == HandshakeState::ChallengeSent) (void)send_handshake(peer, PacketType::Challenge);
            else if (peer.handshake == HandshakeState::ProofSent) (void)send_handshake(peer, PacketType::Proof);
        }
    }

    void service_reliable() {
        for (auto& [_, peer] : peers) {
            for (auto iterator = peer.pendingReliable.begin(); iterator != peer.pendingReliable.end();) {
                PendingReliable& pending = iterator->second;
                if (currentTick - pending.lastSendTick < config.reliableRetryTicks) { ++iterator; continue; }
                if (pending.attempts >= config.maximumReliableAttempts) {
                    ++telemetry.reliableFailures;
                    fail_handshake(peer);
                    peer.pendingReliable.clear();
                    break;
                }
                for (std::size_t i = 0; i < pending.datagrams.size(); ++i)
                    if (!pending.acknowledged[i]) (void)transmit(peer.endpoint, pending.datagrams[i]);
                pending.lastSendTick = currentTick;
                ++pending.attempts;
                ++telemetry.reliableRetransmissions;
                ++iterator;
            }
        }
    }

    void expire_reassemblies() {
        for (auto& [_, peer] : peers) {
            const auto expire = [&](auto& assemblies) {
                for (auto iterator = assemblies.begin(); iterator != assemblies.end();) {
                    if (currentTick - iterator->second.firstTick > config.reassemblyTimeoutTicks) {
                        ++telemetry.reassembliesExpired;
                        iterator = assemblies.erase(iterator);
                    } else ++iterator;
                }
            };
            expire(peer.reliableAssemblies);
            expire(peer.unreliableAssemblies);
        }
    }
};

UdpReplicationTransport::UdpReplicationTransport(UdpReplicationTransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
    std::string validationError;
    if (!impl_->config.validate(&validationError)) { impl_->error = validationError; return; }
    impl_->nonceState = impl_->config.deterministicNonceSeed;
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { impl_->error = "WSAStartup failed"; return; }
    impl_->winsockInitialized = true;
#endif
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == kInvalidSocket) { impl_->error = "UDP socket creation failed"; return; }
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0) {
        impl_->error = "could not set UDP socket nonblocking"; return;
    }
#else
    const int flags = fcntl(impl_->socket, F_GETFL, 0);
    if (flags < 0 || fcntl(impl_->socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        impl_->error = "could not set UDP socket nonblocking"; return;
    }
#endif
    EndpointAddress bindEndpoint;
    if (!resolve_ipv4(impl_->config.bindAddress, impl_->config.bindPort, bindEndpoint)) {
        impl_->error = "could not parse UDP bind address"; return;
    }
    bindEndpoint.known = true;
    if (::bind(impl_->socket, reinterpret_cast<const sockaddr*>(&bindEndpoint.storage), bindEndpoint.length) != 0) {
        impl_->error = "could not bind UDP socket"; return;
    }
    sockaddr_storage actual{};
    socklen_t actualLength = sizeof(actual);
    if (::getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&actual), &actualLength) != 0) {
        impl_->error = "could not query UDP bind port"; return;
    }
    impl_->localPort = ntohs(reinterpret_cast<const sockaddr_in*>(&actual)->sin_port);
    impl_->valid = true;
}

UdpReplicationTransport::~UdpReplicationTransport() = default;
UdpReplicationTransport::UdpReplicationTransport(UdpReplicationTransport&&) noexcept = default;
UdpReplicationTransport& UdpReplicationTransport::operator=(UdpReplicationTransport&&) noexcept = default;
bool UdpReplicationTransport::valid() const noexcept { return impl_ && impl_->valid; }
std::string UdpReplicationTransport::last_error() const { return impl_ ? impl_->error : "transport is moved from"; }
std::uint16_t UdpReplicationTransport::local_port() const noexcept { return impl_ ? impl_->localPort : 0U; }
ReplicationPeerId UdpReplicationTransport::local_peer() const noexcept {
    return impl_ ? impl_->config.localPeer : kInvalidReplicationPeerId;
}

bool UdpReplicationTransport::add_peer(UdpReplicationPeerConfig config, std::string* error) {
    if (!valid() || !config.validate(error) || config.peer == impl_->config.localPeer ||
        impl_->peers.contains(config.peer)) {
        if (error && error->empty()) *error = "UDP peer cannot be added";
        return false;
    }
    PeerState state;
    state.config = std::move(config);
    if (!resolve_ipv4(state.config.address, state.config.port, state.endpoint)) {
        if (error) *error = "could not parse UDP peer address";
        return false;
    }
    if (state.config.allowAuthenticatedPortLearning && state.config.port == 0U) {
        state.endpoint.known = false;
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(&state.endpoint.storage);
        ipv4->sin_port = 0;
    }
    impl_->peers.emplace(state.config.peer, std::move(state));
    return true;
}

bool UdpReplicationTransport::remove_peer(ReplicationPeerId peer) {
    const auto iterator = impl_->peers.find(peer);
    if (iterator == impl_->peers.end()) return false;
    std::fill(iterator->second.config.preSharedKey.begin(), iterator->second.config.preSharedKey.end(), 0U);
    iterator->second.sessionKey.fill(0U);
    impl_->peers.erase(iterator);
    return true;
}

bool UdpReplicationTransport::begin_handshake(ReplicationPeerId peerId, std::string* error) {
    const auto iterator = impl_->peers.find(peerId);
    if (iterator == impl_->peers.end() || !iterator->second.config.initiateHandshake ||
        !iterator->second.endpoint.known) {
        if (error) *error = "UDP peer is not configured as a handshake initiator";
        return false;
    }
    PeerState& peer = iterator->second;
    peer.clientNonce = impl_->random_u64();
    peer.serverNonce = 0U;
    peer.sessionId = 0U;
    peer.sessionKey.fill(0U);
    peer.handshake = HandshakeState::HelloSent;
    peer.handshakeStartTick = impl_->currentTick;
    peer.lastHandshakeTick = 0U;
    peer.handshakeAttempts = 0U;
    ++impl_->telemetry.handshakesStarted;
    if (!impl_->send_handshake(peer, PacketType::Hello)) {
        if (error) *error = "could not send UDP client hello";
        return false;
    }
    return true;
}

bool UdpReplicationTransport::peer_authenticated(ReplicationPeerId peer) const noexcept {
    const auto iterator = impl_->peers.find(peer);
    return iterator != impl_->peers.end() && iterator->second.handshake == HandshakeState::Authenticated;
}

std::optional<UdpReplicationSessionStatus> UdpReplicationTransport::session_status(
    ReplicationPeerId peer) const noexcept {
    const auto iterator = impl_->peers.find(peer);
    if (iterator == impl_->peers.end()) return std::nullopt;
    return UdpReplicationSessionStatus{peer, true,
        iterator->second.handshake == HandshakeState::Authenticated,
        iterator->second.sessionId, iterator->second.authenticatedTick,
        iterator->second.handshakeAttempts};
}

std::optional<std::uint64_t> UdpReplicationTransport::send(
    ReplicationPeerId source, ReplicationPeerId destination,
    ReplicationTransportChannel channel, ReplicationMessageKind kind,
    std::span<const std::uint8_t> payload, std::string* error) {
    if (!valid() || source != impl_->config.localPeer || destination == source || payload.empty()) {
        if (error) *error = "UDP transport send parameters are invalid";
        return std::nullopt;
    }
    const auto iterator = impl_->peers.find(destination);
    if (iterator == impl_->peers.end() || iterator->second.handshake != HandshakeState::Authenticated) {
        if (error) *error = "UDP destination is not authenticated";
        return std::nullopt;
    }
    PeerState& peer = iterator->second;
    if (payload.size() > impl_->config.maximumMessageBytes) {
        if (error) *error = "UDP message exceeds the configured maximum";
        return std::nullopt;
    }
    const std::size_t fragmentCapacity = impl_->config.maximumDatagramBytes - kHeaderBytes - kTagBytes;
    const std::size_t fragmentCountSize = (payload.size() + fragmentCapacity - 1U) / fragmentCapacity;
    if (fragmentCountSize == 0U || fragmentCountSize > kMaximumFragments ||
        (channel == ReplicationTransportChannel::UnreliableSequenced && fragmentCountSize > 1U)) {
        if (error) *error = "UDP unreliable messages must fit in one datagram";
        return std::nullopt;
    }
    if (channel == ReplicationTransportChannel::ReliableOrdered &&
        peer.pendingReliable.size() >= impl_->config.maximumPendingReliableMessages) {
        if (error) *error = "UDP reliable send window is full";
        return std::nullopt;
    }
    const std::uint64_t sequence = channel == ReplicationTransportChannel::ReliableOrdered
        ? peer.nextReliableSequence++ : peer.nextUnreliableSequence++;
    std::vector<std::vector<std::uint8_t>> datagrams;
    datagrams.reserve(fragmentCountSize);
    for (std::size_t fragmentIndex = 0; fragmentIndex < fragmentCountSize; ++fragmentIndex) {
        const std::size_t offset = fragmentIndex * fragmentCapacity;
        const std::size_t count = std::min(fragmentCapacity, payload.size() - offset);
        PacketHeader header;
        header.type = PacketType::Data;
        header.channel = channel;
        header.kind = kind;
        header.source = source;
        header.destination = destination;
        header.sessionId = peer.sessionId;
        header.sequence = sequence;
        header.fragmentIndex = static_cast<std::uint16_t>(fragmentIndex);
        header.fragmentCount = static_cast<std::uint16_t>(fragmentCountSize);
        header.payloadBytes = static_cast<std::uint16_t>(count);
        datagrams.push_back(authenticated_packet(header, payload.subspan(offset, count), peer.sessionKey));
    }
    for (const auto& datagram : datagrams) (void)impl_->transmit(peer.endpoint, datagram);
    ++impl_->telemetry.logicalMessagesQueued;
    if (channel == ReplicationTransportChannel::ReliableOrdered) {
        PendingReliable pending;
        pending.kind = kind;
        pending.sequence = sequence;
        pending.datagrams = std::move(datagrams);
        pending.acknowledged.assign(fragmentCountSize, false);
        pending.lastSendTick = impl_->currentTick;
        pending.attempts = 1U;
        peer.pendingReliable.emplace(sequence, std::move(pending));
        impl_->telemetry.peakPendingReliable = std::max<std::uint64_t>(
            impl_->telemetry.peakPendingReliable, peer.pendingReliable.size());
    }
    return sequence;
}

void UdpReplicationTransport::update() {
    if (!valid()) return;
    ++impl_->currentTick;
    impl_->poll_socket();
    impl_->service_handshakes();
    impl_->service_reliable();
    impl_->expire_reassemblies();
}

std::vector<ReplicationTransportMessage> UdpReplicationTransport::receive(ReplicationPeerId peer) {
    std::vector<ReplicationTransportMessage> output;
    if (!valid() || peer != impl_->config.localPeer) return output;
    auto iterator = impl_->delivered.find(peer);
    if (iterator == impl_->delivered.end()) return output;
    output.reserve(iterator->second.size());
    while (!iterator->second.empty()) {
        output.push_back(std::move(iterator->second.front()));
        iterator->second.pop_front();
    }
    return output;
}

std::uint64_t UdpReplicationTransport::tick() const noexcept { return impl_ ? impl_->currentTick : 0U; }
bool UdpReplicationTransport::idle() const noexcept {
    if (!impl_) return true;
    for (const auto& [_, peer] : impl_->peers)
        if (!peer.pendingReliable.empty() || !peer.reliableAssemblies.empty() ||
            !peer.unreliableAssemblies.empty()) return false;
    return true;
}
const ReplicationTransportTelemetry& UdpReplicationTransport::telemetry() const noexcept {
    return impl_->telemetry;
}
ReplicationTransportCapabilities UdpReplicationTransport::capabilities() const noexcept {
    return {impl_->config.maximumMessageBytes, impl_->config.maximumDatagramBytes,
            true, true, false, true, false};
}

} // namespace dve
