#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/network_runtime_services.hpp"
#include "dve/network_udp.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> shared_key() {
    std::vector<std::uint8_t> key(32U);
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(0x31U + i * 5U);
    return key;
}
}

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("network_transport_v1_48_evidence.json");

        dve::UdpReplicationTransportConfig serverConfig;
        serverConfig.localPeer = 1U;
        serverConfig.bindPort = 0U;
        serverConfig.deterministicNonceSeed = 0x14800001ULL;
        serverConfig.reliableRetryTicks = 3U;
        serverConfig.handshakeRetryTicks = 3U;
        dve::UdpReplicationTransport server(serverConfig);
        dve::UdpReplicationTransportConfig clientConfig = serverConfig;
        clientConfig.localPeer = 2U;
        clientConfig.deterministicNonceSeed = 0x14800002ULL;
        dve::UdpReplicationTransport client(clientConfig);
        require(server.valid(), server.last_error().c_str());
        require(client.valid(), client.last_error().c_str());

        const auto key = shared_key();
        std::string error;
        dve::UdpReplicationPeerConfig serverPeer;
        serverPeer.peer = 2U;
        serverPeer.port = 0U;
        serverPeer.preSharedKey = key;
        serverPeer.allowAuthenticatedPortLearning = true;
        require(server.add_peer(std::move(serverPeer), &error), error.c_str());
        dve::UdpReplicationPeerConfig clientPeer;
        clientPeer.peer = 1U;
        clientPeer.port = server.local_port();
        clientPeer.preSharedKey = key;
        clientPeer.initiateHandshake = true;
        require(client.add_peer(std::move(clientPeer), &error), error.c_str());
        require(client.begin_handshake(1U, &error), error.c_str());
        std::uint32_t handshakeTicks{};
        for (; handshakeTicks < 120U &&
               !(server.peer_authenticated(2U) && client.peer_authenticated(1U)); ++handshakeTicks) {
            server.update();
            client.update();
        }
        require(server.peer_authenticated(2U) && client.peer_authenticated(1U),
                "authenticated UDP session did not complete");

        std::vector<std::uint8_t> checkpoint(64U * 1024U + 113U);
        for (std::size_t i = 0; i < checkpoint.size(); ++i)
            checkpoint[i] = static_cast<std::uint8_t>((i * 29U + 17U) & 0xffU);
        dve::LateJoinCheckpointSender sender;
        require(sender.begin(148U, 2400U, 731U, checkpoint, 16U * 1024U, &error), error.c_str());
        dve::LateJoinCheckpointReceiver receiver;
        dve::ReplicationTransportEndpoint serverEndpoint(server, 1U);
        dve::ReplicationTransportEndpoint clientEndpoint(client, 2U);
        std::uint32_t transferTicks{};
        for (; transferTicks < 1200U && !receiver.complete(); ++transferTicks) {
            (void)sender.pump(serverEndpoint, 2U, 2U, &error);
            server.update();
            client.update();
            for (const auto& message : clientEndpoint.receive())
                if (message.kind == dve::ReplicationMessageKind::CheckpointFragment)
                    require(receiver.accept(message, &error), error.c_str());
        }
        for (int i = 0; i < 8; ++i) { server.update(); client.update(); }
        const auto reconstructed = receiver.take_checkpoint();
        require(reconstructed && *reconstructed == checkpoint, "late-join checkpoint mismatch");

        dve::ServerInputValidator validator;
        require(validator.bind_owned_pawn(2U, 77U, &error), error.c_str());
        const auto validInput = validator.accept(2U, 3000U,
            dve::make_replicated_input_command(77U, 3000U, {{0.7F, 0.2F, 0}, false, false}));
        const dve::ReplicatedInputCommand invalidDiagonal{77U, 3001U, 32767, 32767, 0U};
        const auto invalidInput = validator.accept(2U, 3001U, invalidDiagonal);
        require(validInput.accepted && !invalidInput.accepted,
                "server input validation demonstration failed");

        dve::RemoteCharacterInterpolator interpolator;
        dve::ReplicatedCharacterState first;
        first.pawn = 88U;
        first.position = dve::quantize_position_millimeters({0,0,1});
        first.velocity = dve::quantize_velocity_centimeters({2,0,0});
        dve::ReplicatedCharacterState second = first;
        second.position = dve::quantize_position_millimeters({1.2F,0,1});
        require(interpolator.push(100U, first, &error), error.c_str());
        require(interpolator.push(106U, second, &error), error.c_str());
        const auto sample = interpolator.sample(109.0);
        require(sample.has_value(), "remote interpolation demonstration failed");

        const auto session = server.session_status(2U);
        std::filesystem::create_directories(output.parent_path().empty()
            ? std::filesystem::path(".") : output.parent_path());
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file << "{\n"
             << "  \"version\": \"1.48.0\",\n"
             << "  \"authenticated_session\": true,\n"
             << "  \"session_id\": " << (session ? session->sessionId : 0U) << ",\n"
             << "  \"handshake_ticks\": " << handshakeTicks << ",\n"
             << "  \"checkpoint_bytes\": " << checkpoint.size() << ",\n"
             << "  \"checkpoint_chunks\": " << sender.manifest().chunkCount << ",\n"
             << "  \"checkpoint_transfer_ticks\": " << transferTicks << ",\n"
             << "  \"reliable_messages_acknowledged\": "
             << server.telemetry().reliableMessagesAcknowledged << ",\n"
             << "  \"valid_input_accepted\": true,\n"
             << "  \"invalid_diagonal_rejected\": true,\n"
             << "  \"remote_interpolated_x\": " << sample->position.x << ",\n"
             << "  \"payload_confidentiality_claimed\": false\n"
             << "}\n";
        require(static_cast<bool>(file), "could not write v1.48 evidence");
        std::cout << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.48 network transport demo failed: " << exception.what() << '\n';
        return 1;
    }
}
