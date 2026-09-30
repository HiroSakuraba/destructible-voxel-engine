#pragma once
// Platform-neutral core of dve_player: boots a game from a ContentSource and runs it.
//
//   boot():  game.dvegame -> settings (game_settings.txt, optional) -> input bindings ->
//            GameWorld with the best available 3D physics (Jolt when compiled in, otherwise
//            the reference solver) -> Lua host + startupScript (when built with Lua) ->
//            entry scene via load_scene_into_game_world.
//   loop:    handle_event() for platform events, FixedStepClock for the fixed-step
//            accumulator, tick() per step (input -> world, then GameWorld::tick which also
//            runs Lua on_tick), then render_view() for the renderer.
//
// No SDL (or any window system) here: dve_player wires this to SdlApplicationHost and an
// SDL texture blitter, tests drive it with HeadlessApplicationHost-style events directly.
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/content_source.hpp"
#include "dve/game_manifest.hpp"
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

struct PlayerBootOptions {
    std::optional<std::string> sceneOverride;   // content path; default manifest.entryScene
    bool enableScripts{true};
    Physics3DBackend physicsBackend{Physics3DBackend::Automatic};
    // Scene parents become GameWorld attachments, so children follow their parents
    // (GameSceneLoadOptions::attachChildrenToParents). Objects exported with an attachment
    // extension are attached either way.
    bool attachChildrenToParents{true};
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

private:
    PlayerApp();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::player
