#pragma once
#include "dve/game_save.hpp"
namespace dve {
// Validation checkpoint, not a replacement for backend-specific rollback
// snapshots. Every serialized runtime has its own fingerprint so a divergence
// names the subsystem.
struct RuntimeReplayCheckpoint {
    std::uint64_t tick{};
    std::map<std::string, std::uint64_t, std::less<>> subsystemHashes;
    std::uint64_t combinedHash{};
};
[[nodiscard]] std::optional<RuntimeReplayCheckpoint> capture_runtime_replay_checkpoint(
    const GameWorld& world, const GameSaveCodec& codec,
    std::optional<std::vector<std::byte>> scriptState = std::nullopt,
    std::map<std::string, std::vector<std::byte>, std::less<>> applicationSections = {},
    std::string* error = nullptr);
[[nodiscard]] std::vector<std::string>
compare_runtime_replay_checkpoints(const RuntimeReplayCheckpoint& expected,
                                   const RuntimeReplayCheckpoint& actual);
} // namespace dve
