#pragma once
// Player save games: a GameWorld (objects, destruction state, physics bodies, timers, pools)
// plus opaque script state, stored in the v2.35 DVESAVE1 container (SaveGameStore in
// dve/v235_foundations.hpp), which provides the header, per-section and document hashes,
// size limits, migrations, atomic publish and the `.bak` rotation. See docs/SAVE_GAMES.md.
//
// Schema version 1 sections (all little endian, all bounds-checked on read):
//   dve.meta     engine/game identity, scene, tick count, GameWorld::state_hash()
//   dve.world    ids, names, tags, components, flags, transforms, attachments, sources,
//                mass/material tables, timers, pools
//   dve.voxels   per voxel object: a brick delta against its source asset when possible,
//                otherwise every brick (each brick raw or run-length encoded)
//   dve.physics  per body: previous/current transform, velocities, sleeping, local COM
//   dve.script   opaque bytes from the script host (GameScriptHost::save_state)
//   <game>       any extra section a game adds (names must not start with "dve.")
//
// Loading resolves every source asset through a GameSaveSourceReader (normally the game's
// ContentSource), checks it against the FNV-1a hash recorded at save time, and rebuilds the
// voxels. A save whose sources changed (a patched game) fails with a message naming the asset.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/game_world.hpp"
#include "dve/v235_foundations.hpp"

namespace dve {

inline constexpr std::uint32_t kGameSaveSchemaVersion = 1U;
inline constexpr std::string_view kGameSaveExtension = ".dvesave";

struct GameSaveLimits {
    std::uint64_t maximumFileBytes{256ULL * 1024ULL * 1024ULL};
    std::uint32_t maximumObjects{65536U};
    std::uint64_t maximumTotalBricks{1ULL << 20U};        // 512 MiB of decoded materials
    std::uint32_t maximumStringBytes{4096U};              // names, tags, paths (content-path limit)
    std::uint32_t maximumTagsPerObject{256U};
    std::uint32_t maximumComponentsPerObject{256U};
    std::uint32_t maximumPropertiesPerComponent{256U};
    std::uint32_t maximumMaterialsPerObject{256U};
    std::uint32_t maximumTimers{65536U};
    std::uint32_t maximumPools{4096U};
    std::uint64_t maximumScriptStateBytes{16ULL * 1024ULL * 1024ULL};
    std::uint32_t maximumGameSections{32U};
    std::uint64_t maximumSourceAssetBytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct GameSaveMetadata {
    std::string engineVersion;   // filled with dve::kVersionString when encoding
    std::string gameName;
    std::string gameVersion;
    std::string scenePath;
    std::uint64_t tickCount{};
    std::uint64_t worldStateHash{};   // GameWorld::state_hash() at save time
    std::map<std::string, std::string> info;   // free-form (slot label, play time, ...)
};

struct GameSaveData {
    GameSaveMetadata metadata;
    GameWorldSaveState world;
    std::optional<std::vector<std::byte>> scriptState;
    std::map<std::string, std::vector<std::byte>, std::less<>> gameSections;
};

// Reads a GameObjectSource::path (normally ContentSource::read). Returns nullopt + *error.
using GameSaveSourceReader = std::function<std::optional<std::vector<std::byte>>(
    std::string_view path, std::uint64_t maximumBytes, std::string* error)>;

struct GameSaveStats {
    std::uint64_t fileBytes{};
    std::map<std::string, std::uint64_t> sectionBytes;
    std::size_t objects{};
    std::size_t voxelObjects{};
    std::size_t deltaObjects{};       // stored as a delta against their source asset
    std::size_t fullObjects{};        // stored brick by brick (runtime objects, fragments)
    std::uint64_t deltaBricks{};      // bricks written for delta objects
    std::uint64_t unchangedBricks{};  // source bricks a delta did not need to store
    std::uint64_t fullBricks{};
};

// FNV-1a 64 of an asset's bytes, as recorded in GameObjectSource::contentHash.
[[nodiscard]] std::uint64_t game_save_content_hash(std::span<const std::byte> bytes) noexcept;

class GameSaveCodec {
public:
    explicit GameSaveCodec(GameSaveSourceReader sources = {}, GameSaveLimits limits = {});

    [[nodiscard]] const GameSaveLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] const SaveGameStore& store() const noexcept { return store_; }
    // Games (and tests) register document migrations for older schema versions here; every
    // read and decode runs them before interpreting the sections.
    [[nodiscard]] bool register_migration(
        std::uint32_t fromVersion, SaveGameMigration migration, std::string* error = nullptr);

    // GameSaveData <-> DVESAVE1 document at kGameSaveSchemaVersion.
    [[nodiscard]] std::optional<SaveGameDocument> to_document(
        const GameSaveData& data, GameSaveStats* stats = nullptr, std::string* error = nullptr) const;
    [[nodiscard]] std::optional<GameSaveData> from_document(
        const SaveGameDocument& document, std::string* error = nullptr) const;

    [[nodiscard]] std::optional<std::vector<std::byte>> encode(
        const GameSaveData& data, GameSaveStats* stats = nullptr, std::string* error = nullptr) const;
    // Container checks, migrations, then from_document().
    [[nodiscard]] std::optional<GameSaveData> decode(
        std::span<const std::byte> bytes, SaveGameReadReport* report = nullptr, std::string* error = nullptr) const;

    [[nodiscard]] bool write_file(
        const std::filesystem::path& path, const GameSaveData& data, GameSaveStats* stats = nullptr,
        std::string* error = nullptr) const;
    [[nodiscard]] std::optional<GameSaveData> read_file(
        const std::filesystem::path& path, const SaveGameReadOptions& options = {},
        SaveGameReadReport* report = nullptr, std::string* error = nullptr) const;

private:
    GameSaveSourceReader sources_;
    GameSaveLimits limits_;
    SaveGameStore store_;
};

} // namespace dve
