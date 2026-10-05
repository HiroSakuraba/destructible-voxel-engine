#pragma once

// game.dvegame: the project manifest a shipped game (or the future dve_player) boots from.
//
//   DVE_GAME 1
//   # full-line comments and blank lines are ignored; "  # ..." after a value is a comment
//   name=Demo
//   version=1.0.0
//   entryScene=scenes/main.dvoxscene.json
//   startupScript=scripts/main.lua      # optional; ignored by builds without Lua
//   settings=game_settings.txt          # optional DVE_GAME_SETTINGS defaults
//   camera=0,1.5,6 -> 0,0.5,0           # optional fallback camera (eye -> target)
//   bind.move_x=key:a/-1,key:d/+1       # optional; reserved, see inputBindings
//
// The format is strict like the engine's other formats: exact version, size/line/key limits,
// no unknown or duplicate keys, and every path must be a valid content path (see
// content_source.hpp). The shipped scene format is DVOXSCENE JSON (decision D2), so
// entryScene must name a ".dvoxscene.json" file.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "dve/types.hpp"

namespace dve {

class ContentSource;

inline constexpr std::string_view kGameManifestFileName = "game.dvegame";
inline constexpr std::uint32_t kGameManifestVersion = 1U;
inline constexpr std::size_t kMaximumGameManifestBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumGameManifestLineBytes = 4096U;
inline constexpr std::size_t kMaximumGameManifestNameBytes = 128U;
inline constexpr std::size_t kMaximumGameManifestVersionBytes = 64U;
inline constexpr std::size_t kMaximumGameManifestBindings = 256U;
inline constexpr std::size_t kMaximumGameManifestBindingBytes = 1024U;
inline constexpr std::string_view kDefaultStartupScript = "scripts/main.lua";

struct GameManifestCamera {
    Float3 eye{};
    Float3 target{};
};

struct GameManifest {
    std::string name;                    // required, 1..128 printable bytes
    std::string version;                 // required game version, [A-Za-z0-9._+-], 1..64 bytes
    std::string entryScene;              // required content path to a .dvoxscene.json
    std::optional<std::string> startupScript; // content path; absent => kDefaultStartupScript if present
    std::optional<std::string> settings;      // content path to game_settings.txt-style defaults
    std::optional<GameManifestCamera> camera;
    // Raw `bind.<action>=<spec>` entries keyed by action name ([A-Za-z0-9_.-]). The spec
    // syntax is interpreted by the player's input layer (decision D6, Phase 2); Phase 1 only
    // validates the key and length so manifests written now stay loadable.
    std::map<std::string, std::string> inputBindings;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    // Canonical text (fixed key order); parse(serialize()) round-trips.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<GameManifest> parse(std::string_view text, std::string* error = nullptr);

    // startupScript if set, otherwise kDefaultStartupScript.
    [[nodiscard]] std::string startup_script_or_default() const;
};

// Reads and parses `game.dvegame` from the root of `content`.
[[nodiscard]] std::optional<GameManifest> load_game_manifest(
    const ContentSource& content, std::string* error = nullptr);

// Checks that the files the manifest names actually exist in `content`: entryScene always,
// startupScript/settings when they were given explicitly.
[[nodiscard]] bool check_game_manifest_content(
    const GameManifest& manifest, const ContentSource& content, std::string* error = nullptr);

} // namespace dve
