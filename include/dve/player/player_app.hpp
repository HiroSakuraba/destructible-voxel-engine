#pragma once
// Platform-neutral core of dve_player: boots a game from a ContentSource and runs it.
//
//   boot():  game.dvegame -> settings (game_settings.txt, optional) -> input bindings ->
//            GameWorld with the best available 3D physics (Jolt when compiled in, otherwise
//            the reference solver) -> Lua host + startupScript (when built with Lua) ->
//            entry scene via load_scene_into_game_world.
//   loop:    handle_event() for platform events, FixedStepClock for the fixed-step
//            accumulator, tick() per step (input -> world, then GameWorld::tick which also
//            runs Lua on_tick, then any queued save/load), then render_view() for the renderer.
//   saves:   save_game()/load_game() (dve/game_save.hpp), bound to the reserved `quicksave` /
//            `quickload` actions (bind.quicksave / bind.quickload, default F5 / F9) and to
//            Lua's world.save_game / world.load_game. Slots live in save_directory():
//            $XDG_DATA_HOME/dve/<game>/saves on Linux (docs/SAVE_GAMES.md).
//
// No SDL (or any window system) here: dve_player wires this to SdlApplicationHost and an
// SDL texture blitter, tests drive it with HeadlessApplicationHost-style events directly.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/content_source.hpp"
#include "dve/game_manifest.hpp"
#include "dve/game_save.hpp"
#include "dve/game_ui.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/platform/application_host.hpp"
#include "dve/player/player_input.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render_environment.hpp"

namespace dve::player {

// Process exit codes shared by dve_player and its tests.
inline constexpr int kExitOk = 0;
inline constexpr int kExitRuntimeError = 1;   // renderer/window failure mid-run
inline constexpr int kExitUsage = 2;          // bad command line or no content found
inline constexpr int kExitContentError = 3;   // missing/corrupt pak, bad manifest/scene/script
inline constexpr int kExitHashMismatch = 4;   // --expect-hash / reference comparison failed

// Reserved actions (bound to F5 / F9 unless game.dvegame binds them) and the slot they use.
inline constexpr std::string_view kQuicksaveAction = "quicksave";
inline constexpr std::string_view kQuickloadAction = "quickload";
inline constexpr std::string_view kDefaultQuicksaveBinding = "key:f5";
inline constexpr std::string_view kDefaultQuickloadBinding = "key:f9";
inline constexpr std::string_view kQuicksaveSlot = "quicksave";
inline constexpr std::size_t kMaximumSaveSlotNameBytes = 64U;

// The per-user data directory: $XDG_DATA_HOME (when absolute) or ~/.local/share on Linux and
// other Unix systems, %APPDATA% on Windows, ~/Library/Application Support on macOS. Empty when
// none can be determined (no HOME).
[[nodiscard]] std::filesystem::path user_data_directory();
// "Player Sample" -> "player-sample": lower-case ASCII letters, digits, '-', '_' and '.'
// (never leading), everything else collapsed to '-'. "game" if nothing is left.
[[nodiscard]] std::string save_directory_slug(std::string_view gameName);
// user_data_directory()/dve/<slug>/saves, or "./saves/<slug>" when there is no data dir.
[[nodiscard]] std::filesystem::path default_save_directory(const GameManifest& manifest);
// Slot names: 1..64 bytes of [A-Za-z0-9_.-], not starting with '.'.
[[nodiscard]] bool valid_save_slot_name(std::string_view slot) noexcept;

struct PlayerBootOptions {
    std::optional<std::string> sceneOverride;   // content path; default manifest.entryScene
    bool enableScripts{true};
    Physics3DBackend physicsBackend{Physics3DBackend::Automatic};
    // Where slots are written; default default_save_directory(manifest). Created on first save.
    std::optional<std::filesystem::path> saveDirectory;
    // A slot name or .dvesave path (see PlayerApp::resolve_save) loaded right after boot.
    std::optional<std::string> loadSave;
    // Informational log lines (boot steps, Lua world.log output). Errors go to *error.
    std::function<void(std::string_view)> log;
};

// Fixed-step accumulator with the EditorPlaySession parameters (1/60 s, 0.25 s clamp,
// at most 8 steps per frame).
struct FixedStepClock {
    float fixedDeltaSeconds{1.0F / 60.0F};
    double maximumFrameSeconds{0.25};
    std::uint32_t maximumSteps{8};
    double accumulator{};
    // Adds wall-clock time and returns how many fixed steps to run now.
    [[nodiscard]] std::uint32_t advance(double elapsedSeconds) noexcept;
};

struct PlayerSaveResult {
    std::filesystem::path path;
    GameSaveStats stats;
    std::uint64_t tickCount{};
    std::uint64_t worldStateHash{};
};

struct PlayerLoadResult {
    std::filesystem::path path;
    SaveGameReadReport read;            // backup fallback, migrations, file size
    GameWorldRestoreReport restore;
    std::uint64_t tickCount{};
    std::uint64_t worldStateHash{};
};

// A save or load that tick() performed (quicksave/quickload bindings, Lua requests).
struct PlayerSaveEvent {
    bool load{};
    bool ok{};
    bool fromScript{};
    std::filesystem::path path;
    std::uint64_t fileBytes{};
    std::string message;   // error when !ok
};

class PlayerApp {
public:
    // Global-vector names a script can set (world.set_global_vector) to drive the camera.
    static constexpr std::string_view kCameraEyeGlobal = "player_camera_eye";
    static constexpr std::string_view kCameraTargetGlobal = "player_camera_target";

    [[nodiscard]] static std::unique_ptr<PlayerApp> boot(
        std::unique_ptr<ContentSource> content, const PlayerBootOptions& options, std::string* error);
    ~PlayerApp();
    PlayerApp(const PlayerApp&) = delete;
    PlayerApp& operator=(const PlayerApp&) = delete;

    [[nodiscard]] const ContentSource& content() const noexcept;
    [[nodiscard]] const GameManifest& manifest() const noexcept;
    [[nodiscard]] const ui::GameSettings& settings() const noexcept;
    [[nodiscard]] GameWorld& world() noexcept;
    [[nodiscard]] const GameWorld& world() const noexcept;
    [[nodiscard]] const PlayerInput& input() const noexcept;

    [[nodiscard]] std::string_view physics_backend() const noexcept;
    [[nodiscard]] static bool scripting_compiled_in() noexcept;
    [[nodiscard]] bool script_loaded() const noexcept;
    [[nodiscard]] const std::string& script_path() const noexcept;  // empty if none ran
    [[nodiscard]] const std::string& scene_path() const noexcept;
    [[nodiscard]] const std::string& scene_name() const noexcept;
    [[nodiscard]] std::size_t scene_object_count() const noexcept;

    void handle_event(const platform::PlatformEvent& event);
    [[nodiscard]] bool quit_requested() const noexcept;
    void request_quit() noexcept;

    // One fixed step: pushes input state into the world, then GameWorld::tick(dt).
    void tick(float fixedDeltaSeconds);
    [[nodiscard]] std::uint64_t tick_count() const noexcept;

    // Camera for the given aspect (width / height): the script override if set, else the
    // manifest camera, else a default. Horizontal FOV from ui::GameSettings::fieldOfViewDegrees.
    [[nodiscard]] render::PolygonCamera camera(float aspect) const;
    // The Lua host's environment (scripts can change it) or the engine default.
    [[nodiscard]] RenderEnvironment environment() const;
    // View for a renderer. `objects` must stay alive (and the world unmodified) while the
    // view is in use; pass the storage in so no allocation happens per frame.
    [[nodiscard]] PlayerRenderView render_view(std::vector<GameRenderObject>& objects, float aspect) const;

    [[nodiscard]] std::optional<double> script_global(const std::string& key) const;

    // --- Save games --------------------------------------------------------------------
    [[nodiscard]] const std::filesystem::path& save_directory() const noexcept;
    // A valid slot name maps to save_directory()/<slot>.dvesave; anything containing a path
    // separator or ending in .dvesave is used as a path. nullopt (+ *error) otherwise.
    [[nodiscard]] std::optional<std::filesystem::path> resolve_save(
        std::string_view slotOrPath, std::string* error = nullptr) const;
    // The codec this player uses; games embedding PlayerApp register migrations here.
    [[nodiscard]] GameSaveCodec& save_codec() noexcept;
    // Writes the world, destruction state, physics bodies and script state (atomically, the
    // previous file kept as .bak). Call between ticks.
    [[nodiscard]] std::optional<PlayerSaveResult> save_game(std::string_view slotOrPath, std::string* error = nullptr);
    // Boots a fresh world + script host for the saved scene, restores the save into it,
    // verifies GameWorld::state_hash() against the saved hash, runs the scripts' on_load and
    // only then replaces the running world. On failure the running game is untouched.
    [[nodiscard]] std::optional<PlayerLoadResult> load_game(std::string_view slotOrPath, std::string* error = nullptr);
    // Saves / loads performed by the most recent tick() (cleared at the start of each tick).
    [[nodiscard]] const std::vector<PlayerSaveEvent>& save_events() const noexcept;

private:
    PlayerApp();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::player
