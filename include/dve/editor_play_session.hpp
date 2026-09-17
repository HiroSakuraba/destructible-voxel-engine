#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "dve/editor_materials.hpp"
#include "dve/editor_workspace.hpp"
#include "dve/game_ui.hpp"
#include "dve/physics3d_backend.hpp"

namespace dve::editor {

enum class EditorPlaySessionState : std::uint8_t { Stopped, Running, Paused };

struct EditorPlaySessionConfig {
    float fixedDeltaSeconds{1.0F / 60.0F};
    float maximumAccumulatedSeconds{0.25F};
    std::uint32_t maximumSubstepsPerUpdate{8U};
    float timeScale{1.0F};
    std::filesystem::path projectRoot;
    std::filesystem::path startupScript;
    bool runStartupScriptInPlay{true};
    bool preferProductionPhysics{true};
    Physics3DBackend physicsBackend{Physics3DBackend::Automatic};
    Physics3DWorldConfig physicsWorld{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct EditorPlayInputSnapshot {
    std::map<std::string, bool, std::less<>> actions;
    std::map<std::string, float, std::less<>> axes;
};

struct EditorPlayHudSnapshot {
    std::string interactionPrompt;
    std::optional<ui::ToolWheelEntry> selectedTool;
};

struct EditorPlaySessionTelemetry {
    EditorPlaySessionState state{EditorPlaySessionState::Stopped};
    EditorMode mode{EditorMode::Edit};
    std::uint64_t fixedTickCount{};
    std::uint32_t stepsLastUpdate{};
    double simulatedSeconds{};
    double droppedSeconds{};
    std::size_t editorObjectCount{};
    std::size_t runtimeObjectCount{};
    std::size_t runtimeOnlyObjectCount{};
    bool productionPhysics{};
    Physics3DBackend physicsBackend{Physics3DBackend::Reference};
    bool scriptHostAvailable{};
    bool startupScriptLoaded{};
};

class EditorPlaySession {
public:
    using LogSink = std::function<void(EditorLogLevel, std::string)>;

    EditorPlaySession();
    ~EditorPlaySession();
    EditorPlaySession(const EditorPlaySession&) = delete;
    EditorPlaySession& operator=(const EditorPlaySession&) = delete;

    [[nodiscard]] bool start(
        EditorWorkspace& workspace,
        const EditorMaterialLibrary& materials,
        EditorMode mode,
        EditorPlaySessionConfig config = {},
        LogSink logSink = {},
        std::string* error = nullptr);
    [[nodiscard]] bool update(EditorWorkspace& workspace, float elapsedSeconds, std::string* error = nullptr);
    [[nodiscard]] bool pause(std::string* error = nullptr);
    [[nodiscard]] bool resume(std::string* error = nullptr);
    [[nodiscard]] bool toggle_pause(std::string* error = nullptr);
    [[nodiscard]] bool single_step(EditorWorkspace& workspace, std::string* error = nullptr);
    [[nodiscard]] bool stop(EditorWorkspace& workspace, std::string* error = nullptr);
    [[nodiscard]] bool accept_runtime_changes(EditorWorkspace& workspace, std::string* error = nullptr);

    void set_action_pressed(std::string action, bool pressed);
    void set_axis(std::string axis, float value);
    void clear_input() noexcept;

    [[nodiscard]] bool active() const noexcept { return state_ != EditorPlaySessionState::Stopped; }
    [[nodiscard]] bool paused() const noexcept { return state_ == EditorPlaySessionState::Paused; }
    [[nodiscard]] EditorPlaySessionState state() const noexcept { return state_; }
    [[nodiscard]] EditorMode mode() const noexcept { return mode_; }
    [[nodiscard]] const EditorPlayInputSnapshot& input() const noexcept { return input_; }
    [[nodiscard]] const EditorPlayHudSnapshot& hud() const noexcept { return hud_; }
    [[nodiscard]] const EditorPlaySessionTelemetry& telemetry() const noexcept { return telemetry_; }
    [[nodiscard]] const EditorPlaySessionConfig& config() const noexcept { return config_; }

private:
    class Runtime;
    [[nodiscard]] bool fixed_step(EditorWorkspace& workspace, std::string* error);
    void reset_runtime_state() noexcept;
    void log(EditorLogLevel level, std::string text) const;

    std::unique_ptr<Runtime> runtime_;
    EditorPlaySessionConfig config_{};
    EditorPlayInputSnapshot input_{};
    EditorPlayHudSnapshot hud_{};
    EditorPlaySessionTelemetry telemetry_{};
    LogSink logSink_{};
    EditorPlaySessionState state_{EditorPlaySessionState::Stopped};
    EditorMode mode_{EditorMode::Edit};
    double accumulatorSeconds_{};
};

[[nodiscard]] std::string_view editor_play_session_state_label(EditorPlaySessionState state) noexcept;

} // namespace dve::editor
