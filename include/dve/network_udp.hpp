#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/network_simulation.hpp"

namespace dve {

struct UdpReplicationTransportConfig {
    ReplicationPeerId localPeer{kInvalidReplicationPeerId};
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t bindPort{};
    std::size_t maximumDatagramBytes{1200U};
    std::size_t maximumMessageBytes{4U * 1024U * 1024U};
    std::size_t maximumPendingReliableMessages{4096U};
    std::size_t maximumReassemblies{1024U};
    std::uint32_t reliableRetryTicks{6U};
    std::uint32_t maximumReliableAttempts{64U};
    std::uint32_t handshakeRetryTicks{15U};
    std::uint32_t handshakeTimeoutTicks{600U};
    std::uint32_t reassemblyTimeoutTicks{600U};
    std::uint64_t deterministicNonceSeed{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct UdpReplicationPeerConfig {
    ReplicationPeerId peer{kInvalidReplicationPeerId};
    std::string address{"127.0.0.1"};
    std::uint16_t port{};
    std::vector<std::uint8_t> preSharedKey;
    bool allowAuthenticatedPortLearning{};
    bool initiateHandshake{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct UdpReplicationSessionStatus {
    ReplicationPeerId peer{kInvalidReplicationPeerId};
    bool configured{};
    bool authenticated{};
    std::uint64_t sessionId{};
    std::uint64_t authenticatedTick{};
    std::uint32_t handshakeAttempts{};
};

// A real nonblocking UDP backend for IReplicationTransport. It authenticates peers with a
// pre-shared key and HMAC-SHA-256, derives a per-session key, provides reliable ordered delivery
// with fragmentation/ACK/retransmission, and provides unreliable sequenced snapshots.
// Payload confidentiality is intentionally not claimed; add an encrypted backend or DTLS/QUIC
// wrapper when confidentiality is required.
class UdpReplicationTransport final : public IReplicationTransport {
public:
    explicit UdpReplicationTransport(UdpReplicationTransportConfig config);
    ~UdpReplicationTransport();
    UdpReplicationTransport(const UdpReplicationTransport&) = delete;
    UdpReplicationTransport& operator=(const UdpReplicationTransport&) = delete;
    UdpReplicationTransport(UdpReplicationTransport&&) noexcept;
    UdpReplicationTransport& operator=(UdpReplicationTransport&&) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] ReplicationPeerId local_peer() const noexcept;
    [[nodiscard]] bool add_peer(UdpReplicationPeerConfig peer, std::string* error = nullptr);
    [[nodiscard]] bool remove_peer(ReplicationPeerId peer);
    [[nodiscard]] bool begin_handshake(ReplicationPeerId peer, std::string* error = nullptr);
    [[nodiscard]] bool peer_authenticated(ReplicationPeerId peer) const noexcept;
    [[nodiscard]] std::optional<UdpReplicationSessionStatus> session_status(
        ReplicationPeerId peer) const noexcept;

    [[nodiscard]] std::optional<std::uint64_t> send(
        ReplicationPeerId source, ReplicationPeerId destination,
        ReplicationTransportChannel channel, ReplicationMessageKind kind,
        std::span<const std::uint8_t> payload, std::string* error = nullptr) override;
    void update() override;
    [[nodiscard]] std::vector<ReplicationTransportMessage> receive(
        ReplicationPeerId peer) override;
    [[nodiscard]] std::uint64_t tick() const noexcept override;
    [[nodiscard]] bool idle() const noexcept override;
    [[nodiscard]] const ReplicationTransportTelemetry& telemetry() const noexcept override;
    [[nodiscard]] ReplicationTransportCapabilities capabilities() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve
