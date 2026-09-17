#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dve/network_simulation.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> deterministic_trace() {
    dve::SimulatedNetworkConfig config;
    config.minimumLatencyTicks = 1;
    config.maximumLatencyTicks = 5;
    config.reorderExtraTicks = 5;
    config.lossPermille = 180;
    config.duplicationPermille = 220;
    config.reorderPermille = 600;
    config.maximumDatagramBytes = 256;
    config.bandwidthBytesPerTick = 1024;
    config.reliableRetryTicks = 4;
    config.seed = 0x123456789ULL;
    dve::DeterministicReplicationTransport transport(config);
    for (std::uint8_t index = 0; index < 12U; ++index) {
        const std::vector<std::uint8_t> payload(90U + index, index);
        require(transport.send_reliable(1, 2, dve::ReplicationMessageKind::UserPayload, payload).has_value(),
                "deterministic trace send failed");
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> trace;
    for (int tick = 0; tick < 3000 && trace.size() < 12U; ++tick) {
        transport.advance_tick();
        for (const auto& message : transport.receive(2))
            trace.emplace_back(message.sequence, message.deliveryTick);
    }
    require(trace.size() == 12U, "deterministic trace did not converge");
    return trace;
}

void test_reliable_ordered_under_impairment() {
    dve::SimulatedNetworkConfig config;
    config.minimumLatencyTicks = 1;
    config.maximumLatencyTicks = 6;
    config.reorderExtraTicks = 7;
    config.lossPermille = 300;
    config.duplicationPermille = 250;
    config.reorderPermille = 650;
    config.maximumDatagramBytes = 256;
    config.bandwidthBytesPerTick = 1536;
    config.reliableRetryTicks = 4;
    config.maximumReliableAttempts = 256;
    config.seed = 0xA11CE146ULL;
    dve::DeterministicReplicationTransport transport(config);

    constexpr std::uint64_t count = 64U;
    for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
        std::vector<std::uint8_t> payload(80U + static_cast<std::size_t>(sequence % 17U),
                                          static_cast<std::uint8_t>(sequence));
        require(transport.send_reliable(1, 2, dve::ReplicationMessageKind::DestructionBatch,
                                        payload).has_value(),
                "reliable queue rejected a valid message");
    }

    std::vector<dve::ReplicationTransportMessage> delivered;
    for (int tick = 0; tick < 12000 && delivered.size() < count; ++tick) {
        transport.advance_tick();
        auto batch = transport.receive(2);
        delivered.insert(delivered.end(), std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
    }
    require(delivered.size() == count, "reliable messages did not all arrive");
    for (std::size_t index = 0; index < delivered.size(); ++index) {
        require(delivered[index].sequence == index + 1U, "reliable delivery order changed");
        require(!delivered[index].payload.empty(), "reliable payload was empty");
        require(delivered[index].payload.front() == static_cast<std::uint8_t>(index + 1U),
                "reliable payload was corrupted");
    }
    for (int tick = 0; tick < 1000 && transport.pending_reliable_messages() > 0U; ++tick)
        transport.advance_tick();
    require(transport.pending_reliable_messages() == 0U, "reliable ACK convergence failed");
    require(transport.telemetry().reliableRetransmissions > 0U,
            "lossy reliable test did not exercise retransmission");
    require(transport.telemetry().datagramsDropped > 0U,
            "lossy reliable test did not exercise loss");
    require(transport.telemetry().datagramsDuplicated > 0U,
            "lossy reliable test did not exercise duplication");
}

void test_unreliable_sequenced_discards_stale_packets() {
    dve::SimulatedNetworkConfig config;
    config.minimumLatencyTicks = 1;
    config.maximumLatencyTicks = 3;
    config.reorderExtraTicks = 20;
    config.lossPermille = 0;
    config.duplicationPermille = 400;
    config.reorderPermille = 900;
    config.maximumDatagramBytes = 512;
    config.bandwidthBytesPerTick = 4096;
    config.seed = 0x51A7EULL;
    dve::DeterministicReplicationTransport transport(config);
    for (std::uint8_t index = 1; index <= 50U; ++index) {
        const std::vector<std::uint8_t> payload{index};
        require(transport.send_unreliable(10, 20, dve::ReplicationMessageKind::GameplaySnapshot,
                                          payload).has_value(),
                "unreliable send failed");
    }
    std::vector<std::uint64_t> sequences;
    for (int tick = 0; tick < 200; ++tick) {
        transport.advance_tick();
        for (const auto& message : transport.receive(20)) sequences.push_back(message.sequence);
    }
    require(!sequences.empty(), "no unreliable snapshot arrived");
    require(std::is_sorted(sequences.begin(), sequences.end()),
            "unreliable sequenced delivery moved backwards");
    require(std::adjacent_find(sequences.begin(), sequences.end()) == sequences.end(),
            "unreliable duplicate was delivered twice");
    require(sequences.back() == 50U, "latest unreliable snapshot did not win");
    require(transport.telemetry().staleUnreliableDatagrams > 0U,
            "reordering did not exercise stale-snapshot rejection");
}

void test_fragmented_late_join_checkpoint() {
    dve::SimulatedNetworkConfig config;
    config.minimumLatencyTicks = 1;
    config.maximumLatencyTicks = 4;
    config.lossPermille = 220;
    config.duplicationPermille = 120;
    config.reorderPermille = 500;
    config.reorderExtraTicks = 5;
    config.maximumDatagramBytes = 300;
    config.bandwidthBytesPerTick = 1800;
    config.reliableRetryTicks = 5;
    config.maximumReliableAttempts = 256;
    config.seed = 0xC0FFEE146ULL;
    dve::DeterministicReplicationTransport transport(config);
    std::vector<std::uint8_t> checkpoint(32768U);
    for (std::size_t index = 0; index < checkpoint.size(); ++index)
        checkpoint[index] = static_cast<std::uint8_t>((index * 37U + 11U) & 0xFFU);
    require(transport.send_reliable(1, 99, dve::ReplicationMessageKind::CheckpointFragment,
                                    checkpoint).has_value(),
            "checkpoint send failed");
    std::vector<dve::ReplicationTransportMessage> delivered;
    for (int tick = 0; tick < 16000 && delivered.empty(); ++tick) {
        transport.advance_tick();
        delivered = transport.receive(99);
    }
    require(delivered.size() == 1U, "checkpoint was not delivered exactly once");
    require(delivered.front().payload == checkpoint, "fragmented checkpoint changed bytes");
    require(transport.telemetry().datagramsAttempted > 100U,
            "checkpoint did not exercise fragmentation");
}

void test_input_and_repair_codecs() {
    const dve::CharacterInput source{{0.75F, -0.25F, 0.0F}, true, true};
    const auto command = dve::make_replicated_input_command(44U, 900U, source);
    std::string error;
    const auto bytes = dve::encode_replicated_input_command(command, &error);
    require(!bytes.empty(), error.c_str());
    const auto decoded = dve::decode_replicated_input_command(bytes, &error);
    require(decoded.has_value(), error.c_str());
    const auto restored = dve::decode_replicated_input(*decoded);
    require(std::abs(restored.move.x - 0.75F) < 1.0e-4F,
            "replicated input X changed");
    require(std::abs(restored.move.y + 0.25F) < 1.0e-4F,
            "replicated input Y changed");
    require(restored.jumpPressed && restored.crouchHeld,
            "replicated input flags changed");

    dve::ReplicatedBrickRepairRequest request;
    request.object = 77U;
    request.brick = {-3, 4, 9};
    request.causingEditSequence = 12U;
    request.expectedRevision = 6U;
    request.observedRevision = 5U;
    request.expectedContentHash = 0xABCDEFULL;
    request.observedContentHash = 0x123456ULL;
    const auto repairBytes = dve::encode_brick_repair_request(request, &error);
    require(!repairBytes.empty(), error.c_str());
    const auto repair = dve::decode_brick_repair_request(repairBytes, &error);
    require(repair.has_value(), error.c_str());
    require(repair->brick == request.brick &&
            repair->expectedContentHash == request.expectedContentHash,
            "brick repair request changed during round trip");
}

void test_destruction_tracker_and_repair_transport() {
    dve::ReplicatedDestructionEdit edit;
    edit.object = 88U;
    edit.editSequence = 1U;
    edit.center = dve::quantize_position_millimeters({1,2,3});
    edit.radiusMillimeters = 750U;
    edit.expectedBrickRevision = 17U;
    edit.expectedContentHash = 0xAABBCCDDULL;
    edit.brick = {2,-1,5};
    dve::DestructionReplicaTracker tracker;
    dve::ReplicatedBrickRepairRequest request;
    std::string error;
    require(tracker.accept(edit, 16U, 0xDEADBEEFULL, &request, &error) ==
                dve::DestructionReplicaResult::RepairRequired,
            "forced divergence did not request repair");
    require(request.brick == edit.brick && request.causingEditSequence == 1U,
            "repair request lost causal location");
    require(tracker.accept(edit, 17U, edit.expectedContentHash) ==
                dve::DestructionReplicaResult::Duplicate,
            "duplicate destruction edit was reapplied");
    edit.editSequence = 3U;
    require(tracker.accept(edit, 17U, edit.expectedContentHash, nullptr, &error) ==
                dve::DestructionReplicaResult::SequenceGap,
            "destruction sequence gap was not detected");

    dve::SimulatedNetworkConfig config;
    config.minimumLatencyTicks = 1;
    config.maximumLatencyTicks = 4;
    config.lossPermille = 250;
    config.duplicationPermille = 100;
    config.reorderPermille = 400;
    config.maximumDatagramBytes = 256;
    config.bandwidthBytesPerTick = 1024;
    config.reliableRetryTicks = 3;
    config.seed = 0xB71C146ULL;
    dve::DeterministicReplicationTransport transport(config);
    const auto payload = dve::encode_brick_repair_request(request, &error);
    require(transport.send_reliable(2, 1, dve::ReplicationMessageKind::BrickRepairRequest,
                                    payload).has_value(),
            "repair request transport send failed");
    std::vector<dve::ReplicationTransportMessage> received;
    for (int tick = 0; tick < 3000 && received.empty(); ++tick) {
        transport.advance_tick();
        received = transport.receive(1);
    }
    require(received.size() == 1U, "repair request did not converge reliably");
    const auto receivedRepair = dve::decode_brick_repair_request(received.front().payload, &error);
    require(receivedRepair && receivedRepair->brick == request.brick,
            "transported repair request was malformed");
}

void test_replication_v2_brick_address_and_legacy_decode() {
    dve::GameplayReplicationFrame frame;
    frame.serverTick = 20U;
    frame.baselineTick = 10U;
    frame.destructionEdits.push_back({55U, 1U, dve::quantize_position_millimeters({1,2,3}),
                                      500U, 7U, 0x99887766ULL, {-4,2,11}});
    std::string error;
    auto bytes = dve::encode_gameplay_replication_frame(frame, &error);
    require(!bytes.empty(), error.c_str());
    const auto decoded = dve::decode_gameplay_replication_frame(bytes, &error);
    require(decoded && decoded->destructionEdits.front().brick == dve::Int3{-4,2,11},
            "v2 replication frame lost brick address");

    // Convert the single-edit packet into the v1 wire shape: change version and remove the
    // three trailing int32 brick coordinates. The compatibility decoder must accept it.
    bytes[4] = 1U;
    bytes[5] = 0U;
    bytes.resize(bytes.size() - 12U);
    const auto legacy = dve::decode_gameplay_replication_frame(bytes, &error);
    require(legacy.has_value(), error.c_str());
    require(legacy->destructionEdits.front().brick == dve::Int3{},
            "legacy v1 frame did not default the missing brick address");
}

void test_bounded_prediction_reconciliation() {
    dve::ClientPredictionConfig config;
    config.positionToleranceMeters = 0.05F;
    config.velocityToleranceMetersPerSecond = 0.10F;
    config.hardSnapDistanceMeters = 0.75F;
    config.maximumHistory = 4U;
    dve::ClientPredictionBuffer buffer(config);
    std::string error;
    for (std::uint64_t tick = 10U; tick <= 14U; ++tick) {
        dve::CharacterInput input{{1,0,0}, tick == 11U, false};
        const auto command = dve::make_replicated_input_command(77U, tick, input);
        dve::ReplicatedCharacterState predicted;
        predicted.pawn = 77U;
        predicted.position = dve::quantize_position_millimeters(
            {static_cast<float>(tick) * 0.1F, 0, 0});
        predicted.velocity = dve::quantize_velocity_centimeters({6,0,0});
        require(buffer.push(command, predicted, &error), error.c_str());
    }
    require(buffer.size() == 4U, "prediction history was not bounded");

    dve::ReplicatedCharacterState nearAuthoritative;
    nearAuthoritative.pawn = 77U;
    nearAuthoritative.position = dve::quantize_position_millimeters({1.203F,0,0});
    nearAuthoritative.velocity = dve::quantize_velocity_centimeters({6.04F,0,0});
    const auto near = buffer.reconcile(12U, nearAuthoritative, &error);
    require(near.has_value(), error.c_str());
    require(near->matchingPredictionFound && !near->correctionRequired,
            "small prediction error caused a correction");
    require(near->inputsToReplay.size() == 2U &&
            near->inputsToReplay.front().clientTick == 13U,
            "acknowledged prediction inputs were not trimmed");

    dve::ReplicatedCharacterState divergent = nearAuthoritative;
    divergent.position = dve::quantize_position_millimeters({0.0F,0,0});
    const auto hard = buffer.reconcile(13U, divergent, &error);
    require(hard && hard->correctionRequired && hard->hardSnap,
            "large prediction divergence did not request a hard snap");
    require(hard->inputsToReplay.size() == 1U && hard->inputsToReplay.front().clientTick == 14U,
            "prediction replay tail is wrong");

    dve::ReplicatedCharacterState missing = divergent;
    const auto missingTick = buffer.reconcile(99U, missing, &error);
    require(missingTick && !missingTick->matchingPredictionFound &&
            missingTick->correctionRequired && missingTick->hardSnap,
            "missing authoritative prediction did not fail safely");
}

} // namespace

int main() {
    try {
        require(deterministic_trace() == deterministic_trace(),
                "simulated network trace is not deterministic");
        test_reliable_ordered_under_impairment();
        test_unreliable_sequenced_discards_stale_packets();
        test_fragmented_late_join_checkpoint();
        test_input_and_repair_codecs();
        test_destruction_tracker_and_repair_transport();
        test_replication_v2_brick_address_and_legacy_decode();test_bounded_prediction_reconciliation();
        std::cout << "network simulation tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "network simulation tests failed: " << exception.what() << '\n';
        return 1;
    }
}
