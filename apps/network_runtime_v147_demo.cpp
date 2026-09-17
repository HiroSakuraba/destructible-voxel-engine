#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/network_simulation.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::unique_ptr<dve::VoxelObject> solid_box(int sx, int sy, int sz) {
    auto voxels = std::make_unique<dve::VoxelObject>(1U);
    for (int z = 0; z < sz; ++z)
        for (int y = 0; y < sy; ++y)
            for (int x = 0; x < sx; ++x)
                (void)voxels->set_voxel({x, y, z}, 1U);
    return voxels;
}

dve::GameObjectId add_box(dve::GameWorld& world, const char* name, int sx, int sy, int sz,
                          float voxelSize, dve::Float3 position) {
    dve::GameObjectDesc desc;
    desc.name = name;
    desc.transform = dve::make_rigid_transform(position, {});
    desc.voxelSizeMeters = voxelSize;
    desc.voxels = solid_box(sx, sy, sz);
    desc.dynamic = false;
    std::string error;
    const auto id = world.create_object(std::move(desc), &error);
    require(id != dve::kInvalidGameObjectId, error.c_str());
    return id;
}

dve::GameObjectId add_marker(dve::GameWorld& world, const char* name, dve::Float3 position) {
    dve::GameObjectDesc desc;
    desc.name = name;
    desc.transform = dve::make_rigid_transform(position, {});
    const auto id = world.create_object(std::move(desc));
    require(id != dve::kInvalidGameObjectId, "marker creation failed");
    return id;
}
}

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("network_runtime_v1_47_evidence.json");
        auto clientPhysics = std::make_unique<dve::ReferenceRigidBodyWorld>();
        clientPhysics->set_gravity({0, 0, 0});
        dve::GameWorld client(std::move(clientPhysics));
        const auto floor = add_box(client, "Floor", 32, 32, 1, 0.25F, {-4, -4, 0});
        const auto pawn = add_marker(client, "Pawn", {0, 0, 0.25F});
        std::string error;
        require(client.gameplay().add_character(pawn, {}, &error), error.c_str());
        for (int i = 0; i < 3; ++i) client.tick(1.0F / 60.0F);
        dve::ReplicationIdentityRegistry clientIds;
        require(clientIds.bind_object(pawn, 17U, &error), error.c_str());
        require(clientIds.bind_object(floor, 18U, &error), error.c_str());
        dve::ClientGameplayPrediction prediction(client, clientIds, pawn, 1.0F / 60.0F);
        for (std::uint64_t tick = 100U; tick <= 105U; ++tick)
            require(prediction.predict(tick, {{1, 0, 0}, false, false}, &error), error.c_str());
        dve::ReplicatedCharacterState authority;
        authority.pawn = 17U;
        authority.position = dve::quantize_position_millimeters({0.10F, 0, 0.25F});
        authority.velocity = dve::quantize_velocity_centimeters({1.2F, 0, 0});
        authority.flags = 0x01U;
        authority.support = 18U;
        const auto reconciliation = prediction.reconcile(102U, authority, &error);
        require(reconciliation.has_value(), error.c_str());

        auto authorityPhysics = std::make_unique<dve::ReferenceRigidBodyWorld>();
        authorityPhysics->set_gravity({0, 0, 0});
        dve::GameWorld authorityWorld(std::move(authorityPhysics));
        const auto authorityObject = add_box(authorityWorld, "Authority", 8, 8, 8, 0.25F, {0, 0, 0});
        auto replicaPhysics = std::make_unique<dve::ReferenceRigidBodyWorld>();
        replicaPhysics->set_gravity({0, 0, 0});
        dve::GameWorld replicaWorld(std::move(replicaPhysics));
        const auto replicaObject = add_box(replicaWorld, "Replica", 8, 8, 8, 0.25F, {0, 0, 0});
        dve::ReplicationIdentityRegistry authorityIds;
        dve::ReplicationIdentityRegistry replicaIds;
        require(authorityIds.bind_object(authorityObject, 91U, &error), error.c_str());
        require(replicaIds.bind_object(replicaObject, 91U, &error), error.c_str());
        auto divergent = replicaWorld.voxel_brick_snapshot(replicaObject, {0, 0, 0}).value();
        divergent.materials[0] = dve::kAirMaterial;
        divergent.revision += 1U;
        divergent.contentHash = dve::VoxelObject::brick_content_hash(divergent.materials);
        require(replicaWorld.replace_voxel_brick(replicaObject, divergent, &error), error.c_str());
        const auto expected = authorityWorld.voxel_brick_snapshot(authorityObject, {0, 0, 0}).value();
        dve::ReplicatedBrickRepairRequest request;
        request.object = 91U;
        request.brick = {0, 0, 0};
        request.causingEditSequence = 3U;
        request.expectedRevision = expected.revision;
        request.observedRevision = divergent.revision;
        request.expectedContentHash = expected.contentHash;
        request.observedContentHash = divergent.contentHash;
        const auto repair = dve::build_brick_repair_response(
            authorityWorld, authorityIds, request, &error);
        require(repair.has_value(), error.c_str());
        const auto repairBytes = dve::encode_brick_repair_response(*repair, &error);
        require(dve::apply_brick_repair_response(
                    replicaWorld, replicaIds,
                    *dve::decode_brick_repair_response(repairBytes, &error), &error),
                error.c_str());

        dve::SimulatedNetworkConfig transportConfig;
        transportConfig.minimumLatencyTicks = 1U;
        transportConfig.maximumLatencyTicks = 2U;
        transportConfig.lossPermille = 0U;
        transportConfig.duplicationPermille = 0U;
        transportConfig.reorderPermille = 0U;
        dve::DeterministicReplicationTransport backend(transportConfig);
        dve::IReplicationTransport& neutralTransport = backend;
        dve::ReplicationTransportEndpoint server(neutralTransport, 1U);
        dve::ReplicationTransportEndpoint remote(neutralTransport, 2U);
        require(server.send(2U, dve::ReplicationTransportChannel::ReliableOrdered,
                            dve::ReplicationMessageKind::BrickRepairResponse,
                            repairBytes, &error).has_value(), error.c_str());
        for (int i = 0; i < 8; ++i) neutralTransport.update();
        const auto delivered = remote.receive();
        require(delivered.size() == 1U, "socket-neutral repair delivery failed");

        std::filesystem::create_directories(output.parent_path().empty()
            ? std::filesystem::path(".") : output.parent_path());
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file << "{\n"
             << "  \"version\": \"1.47.0\",\n"
             << "  \"authoritative_correction_applied\": "
             << (reconciliation->authoritativeStateApplied ? "true" : "false") << ",\n"
             << "  \"prediction_inputs_replayed\": " << reconciliation->replayedInputs << ",\n"
             << "  \"prediction_history_after_replay\": " << prediction.history_size() << ",\n"
             << "  \"brick_repair_bytes\": " << repairBytes.size() << ",\n"
             << "  \"brick_repair_hash\": " << repair->authoritativeContentHash << ",\n"
             << "  \"transport_backend_simulated\": "
             << (neutralTransport.capabilities().simulated ? "true" : "false") << ",\n"
             << "  \"transport_messages_delivered\": " << delivered.size() << "\n"
             << "}\n";
        require(static_cast<bool>(file), "could not write v1.47 evidence");
        std::cout << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.47 network runtime demo failed: " << exception.what() << '\n';
        return 1;
    }
}
