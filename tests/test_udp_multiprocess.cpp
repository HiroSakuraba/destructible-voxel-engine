#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "dve/network_udp.hpp"

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
std::vector<std::uint8_t> shared_key() {
    std::vector<std::uint8_t> key(32U);
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(0x51U + i * 3U);
    return key;
}
}

int main() {
#if defined(_WIN32)
    std::cout << "UDP multiprocess smoke is POSIX-only in this build\n";
    return 0;
#else
    int portPipe[2]{};
    if (pipe(portPipe) != 0) return 1;
    const pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        close(portPipe[1]);
        std::uint16_t serverPort{};
        const ssize_t count = read(portPipe[0], &serverPort, sizeof(serverPort));
        close(portPipe[0]);
        if (count != static_cast<ssize_t>(sizeof(serverPort)) || serverPort == 0U) _exit(2);
        dve::UdpReplicationTransportConfig config;
        config.localPeer = 2U;
        config.bindPort = 0U;
        config.handshakeRetryTicks = 2U;
        config.handshakeTimeoutTicks = 300U;
        config.deterministicNonceSeed = 0x2222U;
        dve::UdpReplicationTransport client(config);
        if (!client.valid()) _exit(3);
        dve::UdpReplicationPeerConfig peer;
        peer.peer = 1U;
        peer.port = serverPort;
        peer.preSharedKey = shared_key();
        peer.initiateHandshake = true;
        std::string error;
        if (!client.add_peer(std::move(peer), &error) || !client.begin_handshake(1U, &error)) _exit(4);
        const std::array<std::uint8_t, 6> request{'c','l','i','e','n','t'};
        bool sent{};
        for (int tick = 0; tick < 1000; ++tick) {
            client.update();
            if (!sent && client.peer_authenticated(1U)) {
                if (!client.send(2U, 1U, dve::ReplicationTransportChannel::ReliableOrdered,
                                 dve::ReplicationMessageKind::UserPayload, request, &error)) _exit(5);
                sent = true;
            }
            for (const auto& message : client.receive(2U)) {
                const std::string payload(message.payload.begin(), message.payload.end());
                if (payload == "server") _exit(0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        _exit(6);
    }

    close(portPipe[0]);
    dve::UdpReplicationTransportConfig config;
    config.localPeer = 1U;
    config.bindPort = 0U;
    config.handshakeRetryTicks = 2U;
    config.handshakeTimeoutTicks = 300U;
    config.deterministicNonceSeed = 0x1111U;
    dve::UdpReplicationTransport server(config);
    if (!server.valid()) return 7;
    dve::UdpReplicationPeerConfig peer;
    peer.peer = 2U;
    peer.port = 0U;
    peer.preSharedKey = shared_key();
    peer.allowAuthenticatedPortLearning = true;
    std::string error;
    if (!server.add_peer(std::move(peer), &error)) return 8;
    const std::uint16_t port = server.local_port();
    if (write(portPipe[1], &port, sizeof(port)) != static_cast<ssize_t>(sizeof(port))) return 9;
    close(portPipe[1]);
    const std::array<std::uint8_t, 6> response{'s','e','r','v','e','r'};
    bool responded{};
    for (int tick = 0; tick < 1000 && !responded; ++tick) {
        server.update();
        for (const auto& message : server.receive(1U)) {
            const std::string payload(message.payload.begin(), message.payload.end());
            if (payload == "client") {
                if (!server.send(1U, 2U, dve::ReplicationTransportChannel::ReliableOrdered,
                                 dve::ReplicationMessageKind::UserPayload, response, &error)) return 10;
                responded = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int status{};
    if (waitpid(child, &status, 0) != child) return 11;
    if (!responded || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 12;
    std::cout << "UDP multiprocess smoke passed\n";
    return 0;
#endif
}
