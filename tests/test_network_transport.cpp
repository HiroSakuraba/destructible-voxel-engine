#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/network_crypto.hpp"
#include "dve/network_runtime_services.hpp"
#include "dve/network_udp.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> key_bytes(std::uint8_t seed) {
    std::vector<std::uint8_t> key(32U);
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(seed + i * 7U);
    return key;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

void test_sha256_and_hmac_vectors() {
    const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
    const auto digest = dve::network_sha256(abc);
    require(hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 known vector failed");

    const std::vector<std::uint8_t> hmacKey(20U, 0x0bU);
    const std::array<std::uint8_t, 8> hmacData{'H','i',' ','T','h','e','r','e'};
    const auto hmac = dve::network_hmac_sha256(hmacKey, hmacData);
    require(hex(hmac) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
            "HMAC-SHA-256 known vector failed");
    auto changed = hmac;
    changed[7] ^= 1U;
    require(!dve::network_constant_time_equal(hmac, changed),
            "constant-time digest comparison accepted different values");
}

struct UdpPair {
    dve::UdpReplicationTransport server;
    dve::UdpReplicationTransport client;

    UdpPair()
        : server([] {
            dve::UdpReplicationTransportConfig config;
            config.localPeer = 1U;
            config.bindPort = 0U;
            config.reliableRetryTicks = 3U;
            config.handshakeRetryTicks = 3U;
            config.handshakeTimeoutTicks = 120U;
            config.deterministicNonceSeed = 0x1111222233334444ULL;
            return config;
        }()),
          client([] {
            dve::UdpReplicationTransportConfig config;
            config.localPeer = 2U;
            config.bindPort = 0U;
            config.reliableRetryTicks = 3U;
            config.handshakeRetryTicks = 3U;
            config.handshakeTimeoutTicks = 120U;
            config.deterministicNonceSeed = 0x5555666677778888ULL;
            return config;
        }()) {
        require(server.valid(), server.last_error().c_str());
        require(client.valid(), client.last_error().c_str());
        const auto key = key_bytes(17U);
        dve::UdpReplicationPeerConfig serverPeer;
        serverPeer.peer = 2U;
        serverPeer.address = "127.0.0.1";
        serverPeer.port = 0U;
        serverPeer.preSharedKey = key;
        serverPeer.allowAuthenticatedPortLearning = true;
        std::string error;
        require(server.add_peer(std::move(serverPeer), &error), error.c_str());

        dve::UdpReplicationPeerConfig clientPeer;
        clientPeer.peer = 1U;
        clientPeer.address = "127.0.0.1";
        clientPeer.port = server.local_port();
        clientPeer.preSharedKey = key;
        clientPeer.initiateHandshake = true;
        require(client.add_peer(std::move(clientPeer), &error), error.c_str());
        require(client.begin_handshake(1U, &error), error.c_str());
        for (int i = 0; i < 120 && !(server.peer_authenticated(2U) && client.peer_authenticated(1U)); ++i) {
            server.update();
            client.update();
        }
        require(server.peer_authenticated(2U) && client.peer_authenticated(1U),
                "UDP authenticated handshake did not complete");
    }

    void update(int count = 1) {
        for (int i = 0; i < count; ++i) {
            server.update();
            client.update();
        }
    }
};

void test_udp_authentication_and_delivery() {
    UdpPair pair;
    const auto serverStatus = pair.server.session_status(2U);
    const auto clientStatus = pair.client.session_status(1U);
    require(serverStatus && clientStatus && serverStatus->sessionId == clientStatus->sessionId &&
            serverStatus->sessionId != 0U, "UDP peers did not agree on a session id");

    std::vector<std::uint8_t> large(48U * 1024U);
    for (std::size_t i = 0; i < large.size(); ++i)
        large[i] = static_cast<std::uint8_t>((i * 31U + 9U) & 0xffU);
    std::string error;
    require(pair.server.send(1U, 2U, dve::ReplicationTransportChannel::ReliableOrdered,
                             dve::ReplicationMessageKind::CheckpointFragment, large, &error).has_value(),
            error.c_str());
    std::vector<dve::ReplicationTransportMessage> delivered;
    for (int i = 0; i < 600 && delivered.empty(); ++i) {
        pair.update();
        auto batch = pair.client.receive(2U);
        delivered.insert(delivered.end(), std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
    }
    require(delivered.size() == 1U && delivered.front().payload == large,
            "UDP reliable fragmented payload did not round trip");
    pair.update(4);
    require(pair.server.telemetry().reliableMessagesAcknowledged == 1U,
            "UDP reliable payload was not acknowledged");

    const std::array<std::uint8_t, 4> snapshot{4U, 3U, 2U, 1U};
    require(pair.client.send(2U, 1U, dve::ReplicationTransportChannel::UnreliableSequenced,
                             dve::ReplicationMessageKind::GameplaySnapshot, snapshot, &error).has_value(),
            error.c_str());
    std::vector<dve::ReplicationTransportMessage> snapshots;
    for (int i = 0; i < 60 && snapshots.empty(); ++i) {
        pair.update();
        snapshots = pair.server.receive(1U);
    }
    require(snapshots.size() == 1U && snapshots.front().payload.size() == snapshot.size(),
            "UDP unreliable snapshot did not arrive");
    require(!pair.client.send(2U, 1U, dve::ReplicationTransportChannel::UnreliableSequenced,
                              dve::ReplicationMessageKind::GameplaySnapshot, large, &error),
            "UDP accepted a fragmented unreliable message");
    const auto capabilities = pair.server.capabilities();
    require(!capabilities.simulated && capabilities.authenticated && !capabilities.encrypted,
            "UDP backend capability flags are incorrect");
}

void test_udp_wrong_key_rejected() {
    dve::UdpReplicationTransportConfig serverConfig;
    serverConfig.localPeer = 10U;
    serverConfig.bindPort = 0U;
    serverConfig.handshakeRetryTicks = 2U;
    serverConfig.handshakeTimeoutTicks = 24U;
    serverConfig.deterministicNonceSeed = 1U;
    dve::UdpReplicationTransport server(serverConfig);
    dve::UdpReplicationTransportConfig clientConfig = serverConfig;
    clientConfig.localPeer = 11U;
    clientConfig.deterministicNonceSeed = 2U;
    dve::UdpReplicationTransport client(clientConfig);
    require(server.valid() && client.valid(), "wrong-key UDP fixtures did not bind");
    std::string error;
    dve::UdpReplicationPeerConfig serverPeer;
    serverPeer.peer = 11U;
    serverPeer.port = 0U;
    serverPeer.preSharedKey = key_bytes(1U);
    serverPeer.allowAuthenticatedPortLearning = true;
    require(server.add_peer(std::move(serverPeer), &error), error.c_str());
    dve::UdpReplicationPeerConfig clientPeer;
    clientPeer.peer = 10U;
    clientPeer.port = server.local_port();
    clientPeer.preSharedKey = key_bytes(2U);
    clientPeer.initiateHandshake = true;
    require(client.add_peer(std::move(clientPeer), &error), error.c_str());
    require(client.begin_handshake(10U, &error), error.c_str());
    for (int i = 0; i < 40; ++i) { server.update(); client.update(); }
    require(!server.peer_authenticated(11U) && !client.peer_authenticated(10U),
            "UDP handshake authenticated mismatched pre-shared keys");
    require(server.telemetry().authenticationFailures > 0U,
            "UDP wrong-key handshake did not record authentication failures");
}

void test_server_input_validation() {
    dve::ServerInputValidationConfig config;
    config.maximumCommandsPerServerTick = 2U;
    config.minimumJumpIntervalTicks = 4U;
    dve::ServerInputValidator validator(config);
    std::string error;
    require(validator.bind_owned_pawn(7U, 90U, &error), error.c_str());
    auto valid = dve::make_replicated_input_command(90U, 100U, {{0.6F, 0.4F, 0}, false, false});
    auto result = validator.accept(7U, 100U, valid);
    require(result.accepted, "valid owned input was rejected");
    require(validator.accept(7U, 100U,
        dve::make_replicated_input_command(90U, 101U, {{0,0,0}, true, false})).accepted,
        "first jump input was rejected");
    result = validator.accept(7U, 100U,
        dve::make_replicated_input_command(90U, 102U, {{0,0,0}, false, false}));
    require(!result.accepted && result.rejection == dve::ServerInputRejection::RateLimited,
            "server input rate limit did not reject the third command in one tick");
    result = validator.accept(7U, 101U,
        dve::make_replicated_input_command(91U, 103U, {{0,0,0}, false, false}));
    require(result.rejection == dve::ServerInputRejection::PawnNotOwned,
            "server accepted input for an unowned pawn");
    dve::ReplicatedInputCommand diagonal{90U, 103U, 32767, 32767, 0U};
    result = validator.accept(7U, 101U, diagonal);
    require(result.rejection == dve::ServerInputRejection::MovementOutOfRange,
            "server accepted super-unit diagonal input");
    result = validator.accept(7U, 101U,
        dve::make_replicated_input_command(90U, 104U, {{0,0,0}, true, false}));
    require(result.rejection == dve::ServerInputRejection::JumpRateLimited,
            "server accepted a jump flood");
    result = validator.accept(7U, 101U,
        dve::make_replicated_input_command(90U, 101U, {{0,0,0}, false, false}));
    require(result.rejection == dve::ServerInputRejection::NonMonotonicTick,
            "server accepted a stale input tick");

    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    dve::GameWorld world(std::move(physics));
    dve::GameObjectDesc pawnDesc;
    pawnDesc.name = "Server Pawn";
    const auto pawn = world.create_object(std::move(pawnDesc));
    require(world.gameplay().add_character(pawn, {}, &error), error.c_str());
    dve::ReplicationIdentityRegistry identities;
    require(identities.bind_object(pawn, 190U, &error), error.c_str());
    dve::ServerInputValidator executionValidator;
    require(executionValidator.bind_owned_pawn(8U, 190U, &error), error.c_str());
    const auto execution = dve::validate_and_apply_server_input(
        world, identities, executionValidator, 8U, 200U,
        dve::make_replicated_input_command(190U, 200U, {{0.5F,0,0}, false, true}), &error);
    require(execution.applied && execution.localPawn == pawn,
            "validated server input did not reach the owned live character");
    require(world.gameplay().character(pawn)->stance == dve::CharacterStance::Standing,
            "input application unexpectedly advanced the fixed-step controller");
}

void test_remote_interpolation_and_application() {
    dve::RemoteInterpolationConfig config;
    config.interpolationDelayTicks = 6.0;
    config.maximumExtrapolationTicks = 3.0;
    config.secondsPerTick = 1.0 / 60.0;
    config.teleportDistanceMeters = 10.0F;
    dve::RemoteCharacterInterpolator interpolator(config);
    dve::ReplicatedCharacterState a;
    a.pawn = 44U;
    a.position = dve::quantize_position_millimeters({0,0,1});
    a.velocity = dve::quantize_velocity_centimeters({1,0,0});
    a.flags = 1U;
    dve::ReplicatedCharacterState b = a;
    b.position = dve::quantize_position_millimeters({6,0,1});
    std::string error;
    require(interpolator.push(100U, a, &error), error.c_str());
    require(interpolator.push(106U, b, &error), error.c_str());
    const auto midway = interpolator.sample(109.0); // render tick 103
    require(midway && std::abs(midway->position.x - 3.0F) < 0.002F && !midway->extrapolated,
            "remote interpolation did not sample halfway");
    const auto extrapolated = interpolator.sample(115.0); // render tick 109, 3 ticks beyond
    require(extrapolated && extrapolated->extrapolated &&
            std::abs(extrapolated->position.x - 6.05F) < 0.002F,
            "remote interpolation did not clamp extrapolation");
    dve::ReplicatedCharacterState teleported = b;
    teleported.position = dve::quantize_position_millimeters({20,0,1});
    require(interpolator.push(112U, teleported, &error), error.c_str());
    const auto snap = interpolator.sample(115.0); // target 109 between 106 and 112
    require(snap && snap->teleportSnap && std::abs(snap->position.x - 20.0F) < 0.002F,
            "remote interpolation did not snap a teleport");
    require(!interpolator.push(111U, teleported, &error),
            "remote interpolation accepted a stale snapshot");

    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    dve::GameWorld world(std::move(physics));
    dve::GameObjectDesc pawnDesc;
    pawnDesc.name = "Remote Pawn";
    pawnDesc.transform = dve::make_rigid_transform({0,0,1}, {});
    const auto pawn = world.create_object(std::move(pawnDesc));
    require(pawn != dve::kInvalidGameObjectId, "remote pawn creation failed");
    require(world.gameplay().add_character(pawn, {}, &error), error.c_str());
    dve::ReplicationIdentityRegistry identities;
    require(identities.bind_object(pawn, 44U, &error), error.c_str());
    require(dve::apply_remote_character_sample(world, identities, *snap, &error), error.c_str());
    require(std::abs(world.position(pawn)->x - 20.0F) < 0.002F,
            "remote character sample did not apply to the live world");
}

void test_late_join_over_udp() {
    UdpPair pair;
    std::vector<std::uint8_t> checkpoint(96U * 1024U + 17U);
    for (std::size_t i = 0; i < checkpoint.size(); ++i)
        checkpoint[i] = static_cast<std::uint8_t>((i * 13U + 3U) & 0xffU);
    dve::LateJoinCheckpointSender sender;
    std::string error;
    require(sender.begin(700U, 900U, 123U, checkpoint, 16U * 1024U, &error), error.c_str());
    dve::LateJoinCheckpointReceiver receiver;
    dve::ReplicationTransportEndpoint serverEndpoint(pair.server, 1U);
    dve::ReplicationTransportEndpoint clientEndpoint(pair.client, 2U);
    bool bufferedDelta{};
    for (int iteration = 0; iteration < 1200 && !receiver.complete(); ++iteration) {
        (void)sender.pump(serverEndpoint, 2U, 2U, &error);
        pair.update();
        for (auto& message : clientEndpoint.receive()) {
            if (message.kind == dve::ReplicationMessageKind::CheckpointFragment) {
                require(receiver.accept(message, &error), error.c_str());
                if (receiver.manifest() && !bufferedDelta) {
                    dve::ReplicationTransportMessage delta;
                    delta.source = 1U;
                    delta.destination = 2U;
                    delta.kind = dve::ReplicationMessageKind::DestructionBatch;
                    delta.channel = dve::ReplicationTransportChannel::ReliableOrdered;
                    delta.payload = {9U,8U,7U};
                    require(receiver.buffer_delta(delta, &error), error.c_str());
                    bufferedDelta = true;
                }
            }
        }
    }
    require(sender.enqueued() && receiver.complete() && receiver.progress() == 1.0,
            "late-join checkpoint did not complete over UDP");
    const auto assembled = receiver.take_checkpoint();
    require(assembled && *assembled == checkpoint,
            "late-join checkpoint content did not round trip");
    const auto deltas = receiver.take_buffered_deltas();
    require(deltas.size() == 1U && deltas.front().payload == std::vector<std::uint8_t>({9U,8U,7U}),
            "late-join receiver did not preserve post-baseline deltas");

    auto corruptBegin = dve::LateJoinCheckpointPacket{};
    corruptBegin.type = dve::LateJoinCheckpointPacketType::Begin;
    corruptBegin.manifest = sender.manifest();
    auto bytes = dve::encode_late_join_checkpoint_packet(corruptBegin, &error);
    bytes.back() ^= 1U;
    const auto decoded = dve::decode_late_join_checkpoint_packet(bytes, &error);
    require(decoded && decoded->manifest.contentHash != sender.manifest().contentHash,
            "late-join manifest corruption fixture did not change the hash");
}

} // namespace

int main() {
    try {
        test_sha256_and_hmac_vectors();
        test_udp_authentication_and_delivery();
        test_udp_wrong_key_rejected();
        test_server_input_validation();
        test_remote_interpolation_and_application();
        test_late_join_over_udp();
        std::cout << "network transport tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "network transport tests failed: " << exception.what() << '\n';
        return 1;
    }
}
