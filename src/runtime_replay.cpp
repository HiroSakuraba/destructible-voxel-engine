#include "dve/runtime_replay.hpp"
#include "dve/camera_runtime.hpp"
#if defined(DVE_HAVE_JOLT)
#include "dve/physics_jolt_backend.hpp"
#endif
namespace dve {
namespace {
std::uint64_t bytes_hash(std::span<const std::byte> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace
std::optional<RuntimeReplayCheckpoint> capture_runtime_replay_checkpoint(
    const GameWorld& world, const GameSaveCodec& codec,
    std::optional<std::vector<std::byte>> scriptState,
    std::map<std::string, std::vector<std::byte>, std::less<>> applicationSections,
    std::string* error) {
    GameSaveData data;
    data.world = world.capture_save_state();
    data.runtimes = capture_game_runtime_state(world);
    data.scriptState = std::move(scriptState);
    data.gameSections = std::move(applicationSections);
    const auto document = codec.to_document(data, nullptr, error);
    if (!document)
        return std::nullopt;
    RuntimeReplayCheckpoint checkpoint;
    checkpoint.tick = world.tick_count();
    for (const auto& [name, bytes] : document->sections) {
        // Frame-driven camera smoothing is presentation state, not a simulation
        // oracle.
        if (name == "dve.cameras" || name == "dve.meta")
            continue;
        checkpoint.subsystemHashes.emplace(name, bytes_hash(bytes));
    }
    checkpoint.subsystemHashes.emplace("dve.world-session", world.runtime_state_hash());
    checkpoint.subsystemHashes.emplace("dve.camera-sequences",
                                       world.cameras().sequence_state_hash());
    if (const auto* reference = dynamic_cast<const ReferenceRigidBodyWorld*>(&world.physics()))
        checkpoint.subsystemHashes.emplace("dve.solver", reference->solver_state_hash());
#if defined(DVE_HAVE_JOLT)
    else if (const auto* jolt = dynamic_cast<const JoltRigidBodyWorld*>(&world.physics())) {
        const auto snapshot = jolt->save_snapshot();
        if (!snapshot) {
            if (error)
                *error = "Jolt checkpoint capture failed";
            return std::nullopt;
        }
        std::uint64_t solverHash = bytes_hash(snapshot->solverBytes);
        const auto value = [&]<class T>(T v) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                solverHash ^= bytes[i];
                solverHash *= 1099511628211ULL;
            }
        };
        const auto vector = [&](Float3 v) {
            value(v.x);
            value(v.y);
            value(v.z);
        };
        const auto transform = [&](const RigidTransform& t) {
            vector(t.position);
            value(t.rotation.x);
            value(t.rotation.y);
            value(t.rotation.z);
            value(t.rotation.w);
        };
        value(snapshot->bodyStates.size());
        for (const auto& body : snapshot->bodyStates) {
            transform(body.previousTransform);
            transform(body.currentTransform);
            vector(body.linearVelocity);
            vector(body.angularVelocity);
            value(body.sleeping);
        }
        for (const auto* mask : {&snapshot->liveBodyMask, &snapshot->liveCharacterMask,
                                 &snapshot->liveSoftBodyMask, &snapshot->liveVehicleMask}) {
            value(mask->size());
            for (const auto live : *mask)
                value(live);
        }
        value(snapshot->contactTimeSeconds);
        value(snapshot->characterStates.size());
        for (const auto& character : snapshot->characterStates) {
            transform(character.transform);
            vector(character.linearVelocity);
            vector(character.groundVelocity);
            vector(character.groundNormal);
            value(character.groundBody);
            value(character.groundState);
        }
        checkpoint.subsystemHashes.emplace("dve.solver", solverHash);
        checkpoint.subsystemHashes.emplace("dve.character-solver",
                                           bytes_hash(snapshot->characterSolverBytes));
    }
#endif
    else {
        if (error)
            *error = "backend has no complete replay fingerprint adapter";
        return std::nullopt;
    }
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (!world.deformables().owners().empty() &&
        !checkpoint.subsystemHashes.contains("deformables")) {
        if (error)
            *error = "active deformables require an application checkpoint section";
        return std::nullopt;
    }
#endif
    checkpoint.combinedHash = 1469598103934665603ULL;
    for (const auto& [name, hash] : checkpoint.subsystemHashes) {
        for (const unsigned char c : name) {
            checkpoint.combinedHash ^= c;
            checkpoint.combinedHash *= 1099511628211ULL;
        }
        for (unsigned shift = 0; shift < 64; shift += 8) {
            checkpoint.combinedHash ^= (hash >> shift) & 255U;
            checkpoint.combinedHash *= 1099511628211ULL;
        }
    }
    return checkpoint;
}
std::vector<std::string> compare_runtime_replay_checkpoints(const RuntimeReplayCheckpoint& expected,
                                                            const RuntimeReplayCheckpoint& actual) {
    std::vector<std::string> differences;
    if (expected.tick != actual.tick)
        differences.emplace_back("tick");
    for (const auto& [name, hash] : expected.subsystemHashes) {
        const auto found = actual.subsystemHashes.find(name);
        if (found == actual.subsystemHashes.end() || found->second != hash)
            differences.push_back(name);
    }
    for (const auto& [name, hash] : actual.subsystemHashes) {
        (void)hash;
        if (!expected.subsystemHashes.contains(name))
            differences.push_back(name);
    }
    return differences;
}
} // namespace dve
