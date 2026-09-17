#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "dve/network_simulation.hpp"
#include <algorithm>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("network_simulation_v1_46_evidence.json");
        dve::SimulatedNetworkConfig config;
        config.minimumLatencyTicks = 2U;
        config.maximumLatencyTicks = 7U;
        config.lossPermille = 250U;
        config.duplicationPermille = 120U;
        config.reorderPermille = 500U;
        config.reorderExtraTicks = 8U;
        config.maximumDatagramBytes = 384U;
        config.bandwidthBytesPerTick = 3072U;
        config.reliableRetryTicks = 5U;
        config.maximumReliableAttempts = 256U;
        config.seed = 0xD6E146A5ULL;
        dve::DeterministicReplicationTransport transport(config);

        std::vector<std::uint8_t> checkpoint(24576U);
        for (std::size_t index = 0; index < checkpoint.size(); ++index)
            checkpoint[index] = static_cast<std::uint8_t>((index * 73U + 19U) & 0xFFU);
        require(transport.send_reliable(1U, 2U, dve::ReplicationMessageKind::CheckpointFragment,
                                        checkpoint).has_value(),
                "checkpoint queue failed");
        for (std::uint8_t index = 1U; index <= 30U; ++index) {
            const std::vector<std::uint8_t> snapshot{index};
            require(transport.send_unreliable(1U, 2U, dve::ReplicationMessageKind::GameplaySnapshot,
                                              snapshot).has_value(),
                    "snapshot queue failed");
        }

        bool checkpointDelivered = false;
        std::uint64_t latestSnapshot{};
        std::uint64_t completionTick{};
        for (std::uint64_t tick = 0U; tick < 20000U && !checkpointDelivered; ++tick) {
            transport.advance_tick();
            for (const auto& message : transport.receive(2U)) {
                if (message.kind == dve::ReplicationMessageKind::CheckpointFragment) {
                    require(message.payload == checkpoint, "checkpoint changed bytes");
                    checkpointDelivered = true;
                    completionTick = transport.tick();
                } else if (message.kind == dve::ReplicationMessageKind::GameplaySnapshot &&
                           !message.payload.empty()) {
                    latestSnapshot = std::max<std::uint64_t>(latestSnapshot, message.payload.front());
                }
            }
        }
        require(checkpointDelivered, "checkpoint did not converge");
        for (int tick = 0; tick < 1000 && transport.pending_reliable_messages() > 0U; ++tick)
            transport.advance_tick();
        require(transport.pending_reliable_messages() == 0U, "ACK convergence failed");

        dve::ReplicatedDestructionEdit edit;
        edit.object = 91U;
        edit.editSequence = 1U;
        edit.center = dve::quantize_position_millimeters({2.0F, 3.0F, 4.0F});
        edit.radiusMillimeters = 650U;
        edit.expectedBrickRevision = 12U;
        edit.expectedContentHash = 0xBADC0FFEEULL;
        edit.brick = {4, -2, 7};
        dve::DestructionReplicaTracker tracker;
        dve::ReplicatedBrickRepairRequest repair;
        std::string error;
        const auto repairResult = tracker.accept(edit, 11U, 0x1234ULL, &repair, &error);
        require(repairResult == dve::DestructionReplicaResult::RepairRequired,
                "divergence did not create repair request");

        dve::ClientPredictionBuffer prediction;
        for (std::uint64_t tick = 100U; tick <= 104U; ++tick) {
            const auto input = dve::make_replicated_input_command(
                17U, tick, {{1.0F, 0.0F, 0.0F}, tick == 101U, false});
            dve::ReplicatedCharacterState predicted;
            predicted.pawn = 17U;
            predicted.position = dve::quantize_position_millimeters(
                {static_cast<float>(tick - 100U) * 0.1F, 0.0F, 0.0F});
            predicted.velocity = dve::quantize_velocity_centimeters({6.0F, 0.0F, 0.0F});
            require(prediction.push(input, predicted, &error), error.c_str());
        }
        dve::ReplicatedCharacterState authoritative;
        authoritative.pawn = 17U;
        authoritative.position = dve::quantize_position_millimeters({0.05F, 0.0F, 0.0F});
        authoritative.velocity = dve::quantize_velocity_centimeters({5.0F, 0.0F, 0.0F});
        const auto reconciliation = prediction.reconcile(102U, authoritative, &error);
        require(reconciliation && reconciliation->correctionRequired,
                "prediction divergence was not detected");

        std::filesystem::create_directories(output.parent_path().empty()
            ? std::filesystem::path(".") : output.parent_path());
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        const auto& telemetry = transport.telemetry();
        file << "{\n"
             << "  \"version\": \"1.46.0\",\n"
             << "  \"checkpoint_bytes\": " << checkpoint.size() << ",\n"
             << "  \"checkpoint_delivery_tick\": " << completionTick << ",\n"
             << "  \"latest_unreliable_snapshot\": " << latestSnapshot << ",\n"
             << "  \"datagrams_attempted\": " << telemetry.datagramsAttempted << ",\n"
             << "  \"datagrams_dropped\": " << telemetry.datagramsDropped << ",\n"
             << "  \"datagrams_duplicated\": " << telemetry.datagramsDuplicated << ",\n"
             << "  \"reliable_retransmissions\": " << telemetry.reliableRetransmissions << ",\n"
             << "  \"stale_unreliable_datagrams\": " << telemetry.staleUnreliableDatagrams << ",\n"
             << "  \"repair_brick\": [" << repair.brick.x << ", " << repair.brick.y
             << ", " << repair.brick.z << "],\n"
             << "  \"prediction_position_error_meters\": "
             << reconciliation->positionErrorMeters << ",\n"
             << "  \"prediction_inputs_to_replay\": "
             << reconciliation->inputsToReplay.size() << "\n"
             << "}\n";
        require(static_cast<bool>(file), "could not write evidence file");
        std::cout << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.46 network simulation demo failed: " << exception.what() << '\n';
        return 1;
    }
}
