#include <cmath>
#include <cstdint>
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

std::unique_ptr<dve::VoxelObject> solid_box(
    int sx, int sy, int sz, dve::MaterialId material = 1) {
    auto voxels = std::make_unique<dve::VoxelObject>(static_cast<std::uint64_t>(material));
    for (int z = 0; z < sz; ++z)
        for (int y = 0; y < sy; ++y)
            for (int x = 0; x < sx; ++x)
                (void)voxels->set_voxel({x, y, z}, material);
    return voxels;
}

dve::GameObjectId add_box(
    dve::GameWorld& world, const char* name, int sx, int sy, int sz,
    float voxelSize, dve::Float3 position, bool dynamic = false) {
    dve::GameObjectDesc desc;
    desc.name = name;
    desc.voxelSizeMeters = voxelSize;
    desc.transform = dve::make_rigid_transform(position, {});
    desc.voxels = solid_box(sx, sy, sz);
    desc.dynamic = dynamic;
    std::string error;
    const auto id = world.create_object(std::move(desc), &error);
    require(id != dve::kInvalidGameObjectId, error.c_str());
    return id;
}

dve::GameObjectId add_marker(
    dve::GameWorld& world, const char* name, dve::Float3 position) {
    dve::GameObjectDesc desc;
    desc.name = name;
    desc.transform = dve::make_rigid_transform(position, {});
    const auto id = world.create_object(std::move(desc));
    require(id != dve::kInvalidGameObjectId, "marker creation failed");
    return id;
}

struct CharacterFixture {
    std::unique_ptr<dve::GameWorld> world;
    dve::GameObjectId floor{};
    dve::GameObjectId pawn{};
    dve::ReplicationIdentityRegistry identities;
};

CharacterFixture make_character_fixture() {
    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    physics->set_gravity({0, 0, 0});
    auto world = std::make_unique<dve::GameWorld>(std::move(physics));
    const auto floor = add_box(*world, "Floor", 32, 32, 1, 0.25F, {-4, -4, 0});
    const auto pawn = add_marker(*world, "Pawn", {0, 0, 0.25F});
    std::string error;
    require(world->gameplay().add_character(pawn, {}, &error), error.c_str());
    for (int i = 0; i < 3; ++i) world->tick(1.0F / 60.0F);
    require(world->gameplay().character(pawn)->grounded, "prediction pawn did not ground");
    CharacterFixture fixture;
    fixture.world = std::move(world);
    fixture.floor = floor;
    fixture.pawn = pawn;
    fixture.identities.bind_object(pawn, 100U, &error);
    fixture.identities.bind_object(floor, 101U, &error);
    return fixture;
}

void test_prediction_execution_and_replay() {
    auto actual = make_character_fixture();
    dve::ClientPredictionConfig config;
    config.positionToleranceMeters = 0.01F;
    config.velocityToleranceMetersPerSecond = 0.01F;
    config.hardSnapDistanceMeters = 1.0F;
    dve::ClientGameplayPrediction prediction(
        *actual.world, actual.identities, actual.pawn, 1.0F / 60.0F, config);
    require(prediction.network_pawn() == 100U, "prediction did not use bound network identity");

    std::string error;
    std::vector<dve::CharacterInput> inputs;
    for (std::uint64_t tick = 10U; tick <= 15U; ++tick) {
        dve::CharacterInput input{{1.0F, tick >= 14U ? 0.25F : 0.0F, 0.0F}, false, false};
        inputs.push_back(input);
        require(prediction.predict(tick, input, &error), error.c_str());
    }

    dve::ReplicatedCharacterState authority;
    authority.pawn = 100U;
    authority.position = dve::quantize_position_millimeters({0.05F, 0.0F, 0.25F});
    authority.velocity = dve::quantize_velocity_centimeters({1.0F, 0.0F, 0.0F});
    authority.flags = 0x01U;
    authority.support = 101U;
    const auto reconciled = prediction.reconcile(12U, authority, &error);
    require(reconciled.has_value(), error.c_str());
    require(reconciled->authoritativeStateApplied, "authoritative state was not applied");
    require(reconciled->replayedInputs == 3U, "unacknowledged prediction tail was not replayed");
    require(prediction.history_size() == 3U, "replayed prediction history was not rebuilt");

    auto expected = make_character_fixture();
    require(expected.world->gameplay().apply_authoritative_character_state(
                expected.pawn, {0.05F, 0.0F, 0.25F}, {1.0F, 0.0F, 0.0F}, true,
                expected.floor, dve::CharacterStance::Standing, 12U, &error),
            error.c_str());
    for (std::uint64_t tick = 13U; tick <= 15U; ++tick) {
        require(expected.world->gameplay().simulate_character_input(
                    expected.pawn, inputs[static_cast<std::size_t>(tick - 10U)],
                    1.0F / 60.0F, tick, &error),
                error.c_str());
    }
    const auto expectedPosition = expected.world->position(expected.pawn).value();
    const auto* expectedState = expected.world->gameplay().character(expected.pawn);
    require(dve::length(dve::subtract(reconciled->finalPosition, expectedPosition)) < 1.0e-5F,
            "reconciliation replay position diverged from direct simulation");
    require(dve::length(dve::subtract(reconciled->finalVelocity, expectedState->velocity)) < 1.0e-5F,
            "reconciliation replay velocity diverged from direct simulation");

    require(prediction.predict(16U, {{0.5F, 0.0F, 0.0F}, false, false}, &error), error.c_str());
    const auto frame = dve::build_gameplay_replication_frame(
        *actual.world, actual.identities, 16U, 15U);
    require(frame.characters.size() == 1U, "prediction frame did not contain the pawn");
    const auto matching = prediction.reconcile(16U, frame.characters.front(), &error);
    require(matching && !matching->reconciliation.correctionRequired &&
            !matching->authoritativeStateApplied && matching->replayedInputs == 0U,
            "matching prediction performed an unnecessary correction");
}

void test_authoritative_brick_repair_round_trip() {
    auto make_world = [] {
        auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
        physics->set_gravity({0, 0, 0});
        auto world = std::make_unique<dve::GameWorld>(std::move(physics));
        const auto object = add_box(*world, "Repairable", 8, 8, 8, 0.25F, {0, 0, 0});
        return std::pair{std::move(world), object};
    };
    auto [authority, authorityObject] = make_world();
    auto [replica, replicaObject] = make_world();
    dve::ReplicationIdentityRegistry authorityIds;
    dve::ReplicationIdentityRegistry replicaIds;
    std::string error;
    require(authorityIds.bind_object(authorityObject, 500U, &error), error.c_str());
    require(replicaIds.bind_object(replicaObject, 500U, &error), error.c_str());

    auto divergent = replica->voxel_brick_snapshot(replicaObject, {0, 0, 0}).value();
    divergent.materials[0] = dve::kAirMaterial;
    divergent.revision += 7U;
    divergent.contentHash = dve::VoxelObject::brick_content_hash(divergent.materials);
    require(replica->replace_voxel_brick(replicaObject, divergent, &error), error.c_str());

    const auto authoritySnapshot = authority->voxel_brick_snapshot(authorityObject, {0, 0, 0}).value();
    const auto replicaSnapshot = replica->voxel_brick_snapshot(replicaObject, {0, 0, 0}).value();
    require(authoritySnapshot.contentHash != replicaSnapshot.contentHash,
            "repair fixture did not diverge");

    dve::ReplicatedBrickRepairRequest request;
    request.object = 500U;
    request.brick = {0, 0, 0};
    request.causingEditSequence = 9U;
    request.expectedRevision = authoritySnapshot.revision;
    request.observedRevision = replicaSnapshot.revision;
    request.expectedContentHash = authoritySnapshot.contentHash;
    request.observedContentHash = replicaSnapshot.contentHash;
    const auto response = dve::build_brick_repair_response(
        *authority, authorityIds, request, &error);
    require(response.has_value(), error.c_str());
    const auto bytes = dve::encode_brick_repair_response(*response, &error);
    require(bytes.size() > 512U, error.c_str());
    const auto decoded = dve::decode_brick_repair_response(bytes, &error);
    require(decoded.has_value(), error.c_str());
    require(dve::apply_brick_repair_response(*replica, replicaIds, *decoded, &error),
            error.c_str());

    const auto repaired = replica->voxel_brick_snapshot(replicaObject, {0, 0, 0}).value();
    require(repaired.revision == authoritySnapshot.revision,
            "brick repair did not preserve authority revision");
    require(repaired.contentHash == authoritySnapshot.contentHash &&
            repaired.materials == authoritySnapshot.materials,
            "brick repair did not reproduce authority materials");
    const auto hit = replica->raycast({0.125F, 0.125F, -1.0F}, {0, 0, 1}, 4.0F);
    require(hit && hit->objectId == replicaObject,
            "brick repair did not rebuild static collision");

    auto dynamicPhysics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    dynamicPhysics->set_gravity({0, 0, 0});
    dve::GameWorld dynamicWorld(std::move(dynamicPhysics));
    const auto dynamicObject = add_box(
        dynamicWorld, "Dynamic Repair", 8, 8, 8, 0.25F, {3, 0, 0}, true);
    require(dynamicWorld.set_linear_velocity(dynamicObject, {2.0F, -1.0F, 0.5F}),
            "dynamic repair fixture velocity setup failed");
    auto dynamicSnapshot = dynamicWorld.voxel_brick_snapshot(dynamicObject, {0, 0, 0}).value();
    dynamicSnapshot.materials[1] = dve::kAirMaterial;
    ++dynamicSnapshot.revision;
    dynamicSnapshot.contentHash = dve::VoxelObject::brick_content_hash(dynamicSnapshot.materials);
    require(dynamicWorld.replace_voxel_brick(dynamicObject, dynamicSnapshot, &error), error.c_str());
    const auto preservedVelocity = dynamicWorld.linear_velocity(dynamicObject);
    require(preservedVelocity &&
            dve::length(dve::subtract(*preservedVelocity, {2.0F, -1.0F, 0.5F})) < 1.0e-5F,
            "dynamic brick repair did not preserve rigid-body velocity");

    auto corrupt = bytes;
    corrupt.back() ^= 0x01U;
    require(!dve::decode_brick_repair_response(corrupt, &error).has_value(),
            "corrupt brick repair payload passed hash validation");
}

void test_socket_neutral_endpoint_contract() {
    dve::SimulatedNetworkConfig config;
    config.minimumLatencyTicks = 1U;
    config.maximumLatencyTicks = 1U;
    config.lossPermille = 0U;
    config.duplicationPermille = 0U;
    config.reorderPermille = 0U;
    dve::DeterministicReplicationTransport backend(config);
    dve::IReplicationTransport& transport = backend;
    dve::ReplicationTransportEndpoint server(transport, 1U);
    dve::ReplicationTransportEndpoint client(transport, 2U);
    const std::vector<std::uint8_t> payload{4U, 5U, 6U};
    std::string error;
    require(server.send(2U, dve::ReplicationTransportChannel::ReliableOrdered,
                        dve::ReplicationMessageKind::SpawnDespawn, payload, &error).has_value(),
            error.c_str());
    for (int i = 0; i < 8; ++i) transport.update();
    const auto received = client.receive();
    require(received.size() == 1U && received.front().payload == payload,
            "socket-neutral endpoint did not deliver through the simulator backend");
    const auto caps = transport.capabilities();
    require(caps.reliableOrdered && caps.unreliableSequenced && caps.simulated,
            "transport capabilities are incomplete");
    require(!server.send(1U, dve::ReplicationTransportChannel::ReliableOrdered,
                         dve::ReplicationMessageKind::UserPayload, payload, &error).has_value(),
            "endpoint accepted a send to itself");
}

} // namespace

int main() {
    try {
        test_prediction_execution_and_replay();
        test_authoritative_brick_repair_round_trip();
        test_socket_neutral_endpoint_contract();
        std::cout << "network runtime tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "network runtime tests failed: " << exception.what() << '\n';
        return 1;
    }
}
