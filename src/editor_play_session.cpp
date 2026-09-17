#include "dve/editor_play_session.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <system_error>
#include <utility>
#include <vector>

#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/rigid_body_adapter.hpp"
#ifdef DVE_HAVE_LUA
#include "dve/game_script.hpp"
#endif

namespace dve::editor {
namespace {

[[nodiscard]] bool finite_positive(float value) noexcept {
    return value > 0.0F && std::isfinite(value);
}

[[nodiscard]] std::filesystem::path resolve_project_path(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || projectRoot.empty()) return path;
    return projectRoot / path;
}

[[nodiscard]] double editor_object_density(
    const EditorObject& object,
    const EditorMaterialLibrary& materials) {
    if (!object.voxels) return 1000.0;
    long double weightedDensity = 0.0L;
    std::uint64_t weightedCount = 0U;
    for (const auto& [key, brick] : object.voxels->bricks()) {
        (void)key;
        if (brick.empty()) continue;
        const auto dense = brick.materials();
        for (const MaterialId material : dense) {
            if (material == kAirMaterial) continue;
            const EditorMaterialEntry* entry = materials.find(material);
            const double density = entry
                ? static_cast<double>(entry->definition.densityKilogramsPerCubicMeter)
                : 1000.0;
            if (density > 0.0 && std::isfinite(density)) {
                weightedDensity += static_cast<long double>(density);
                ++weightedCount;
            }
        }
    }
    if (weightedCount == 0U) return 1000.0;
    return static_cast<double>(weightedDensity / static_cast<long double>(weightedCount));
}

} // namespace

class EditorPlaySession::Runtime {
public:
    explicit Runtime(LogSink sink) : logSink_(std::move(sink)) {}

    [[nodiscard]] bool start(
        EditorDocument& document,
        const EditorMaterialLibrary& materials,
        EditorMode mode,
        const EditorPlaySessionConfig& config,
        EditorPlaySessionTelemetry& telemetry,
        std::string* error) {
        if (world_) {
            if (error) *error = "preview runtime is already active";
            return false;
        }

        const Physics3DBackend requestedBackend = config.preferProductionPhysics
            ? config.physicsBackend
            : Physics3DBackend::Reference;
        Physics3DBackend resolvedBackend = Physics3DBackend::Reference;
        std::string physicsError;
        std::unique_ptr<IRigidBodyWorld> physics = create_physics3d_world(
            requestedBackend, &resolvedBackend, &physicsError, config.physicsWorld);
        if (!physics) {
            if (error) *error = physicsError.empty()
                ? "could not create the selected 3D physics backend"
                : physicsError;
            return false;
        }
        telemetry.physicsBackend = resolvedBackend;
        telemetry.productionPhysics = resolvedBackend != Physics3DBackend::Reference;
        world_ = std::make_unique<GameWorld>(std::move(physics));

        for (const auto& [editorId, object] : document.objects()) {
            std::string objectError;
            GameObjectId runtimeId = kInvalidGameObjectId;
            const std::filesystem::path source = resolve_project_path(config.projectRoot, object.sourceAsset);
            const std::string extension = source.extension().string();
            if (object.flags.collisionEnabled && !source.empty() &&
                (extension == ".dmesh" || extension == ".DMESH") &&
                std::filesystem::exists(source)) {
                const RigidTransform worldTransform = document.world_transform(editorId).value_or(object.transform);
                runtimeId = world_->spawn_polygon_asset(
                    source, object.name, worldTransform,
                    !object.flags.anchored || object.attachment.has_value(),
                    object.flags.structural, &objectError);
            } else {
                GameObjectDesc desc;
                desc.name = object.name;
                desc.tags = object.tags;
                desc.groups = object.groups;
                desc.layer = object.layer;
                desc.components = object.components;
                desc.enabled = object.flags.visible;
                desc.transform = document.world_transform(editorId).value_or(object.transform);
                desc.voxelSizeMeters = object.voxelSizeMeters;
                desc.dynamic = !object.flags.anchored || object.attachment.has_value();
                desc.structural = object.flags.structural;
                desc.densityKilogramsPerCubicMeter = editor_object_density(object, materials);
                if (object.flags.collisionEnabled && object.voxels &&
                    object.voxels->occupied_voxel_count() > 0U) {
                    desc.voxels = clone_voxel_object(*object.voxels);
                }
                runtimeId = world_->create_object(std::move(desc), &objectError);
            }
            if (runtimeId == kInvalidGameObjectId) {
                if (error) *error = "could not create runtime object '" + object.name + "': " + objectError;
                stop();
                return false;
            }
            if (extension == ".dmesh" || extension == ".DMESH") {
                bool hasMembership = false;
                for (const Component& component : object.components) {
                    hasMembership = hasMembership || component.type == "dve.membership";
                    std::string componentError;
                    if (!world_->add_component(runtimeId, component, &componentError)) {
                        if (error) *error = "could not publish component on '" + object.name + "': " + componentError;
                        stop();
                        return false;
                    }
                }
                if (!hasMembership) {
                    Component membership;
                    membership.type = "dve.membership";
                    membership.properties.emplace("tags", join_membership_values(object.tags));
                    membership.properties.emplace("groups", join_membership_values(object.groups));
                    membership.properties.emplace("layer", static_cast<std::int64_t>(object.layer));
                    if (!world_->add_component(runtimeId, std::move(membership), &objectError)) {
                        if (error) *error = "could not publish membership on '" + object.name + "': " + objectError;
                        stop();
                        return false;
                    }
                }
                (void)world_->set_enabled(runtimeId, object.flags.visible);
            }
            editorToRuntime_.emplace(editorId, runtimeId);
        }

        for (const auto& [editorId, object] : document.objects()) {
            if (!object.parent || !object.attachment) continue;
            const auto child = editorToRuntime_.find(editorId);
            const auto parent = editorToRuntime_.find(*object.parent);
            if (child == editorToRuntime_.end() || parent == editorToRuntime_.end()) continue;
            std::string attachmentError;
            if (!world_->attach_object(
                    child->second, parent->second, true, object.attachment->socket,
                    object.attachment->inheritPosition, object.attachment->inheritRotation,
                    &attachmentError)) {
                if (error) *error = "could not publish attachment for '" + object.name + "': " + attachmentError;
                stop();
                return false;
            }
        }

#ifdef DVE_HAVE_LUA
        telemetry.scriptHostAvailable = true;
        if (mode == EditorMode::Play && config.runStartupScriptInPlay) {
            std::filesystem::path startup = resolve_project_path(config.projectRoot, config.startupScript);
            if (startup.empty() && !config.projectRoot.empty()) startup = config.projectRoot / "scripts" / "main.lua";
            if (!startup.empty() && std::filesystem::exists(startup)) {
                scripts_ = std::make_unique<GameScriptHost>(*world_);
                scripts_->set_log_sink([this](bool isError, std::string text) {
                    if (logSink_) logSink_(isError ? EditorLogLevel::Error : EditorLogLevel::Info,
                                           std::move(text));
                });
                std::string scriptError;
                if (!scripts_->run_file(startup, &scriptError)) {
                    if (error) *error = "startup script failed: " + scriptError;
                    stop();
                    return false;
                }
                telemetry.startupScriptLoaded = true;
            }
        }
#else
        telemetry.scriptHostAvailable = false;
        if (mode == EditorMode::Play && config.runStartupScriptInPlay) {
            std::filesystem::path startup = resolve_project_path(config.projectRoot, config.startupScript);
            if (startup.empty() && !config.projectRoot.empty()) startup = config.projectRoot / "scripts" / "main.lua";
            if (!startup.empty() && std::filesystem::exists(startup) && logSink_) {
                logSink_(EditorLogLevel::Warning,
                         "Play session found " + startup.string() +
                         " but this build has DVE_ENABLE_LUA=OFF; physics and input still run");
            }
        }
#endif
        telemetry.editorObjectCount = editorToRuntime_.size();
        telemetry.runtimeObjectCount = world_->object_count();
        telemetry.runtimeOnlyObjectCount = telemetry.runtimeObjectCount > telemetry.editorObjectCount
            ? telemetry.runtimeObjectCount - telemetry.editorObjectCount : 0U;
        return true;
    }

    [[nodiscard]] bool fixed_step(
        EditorDocument& document,
        const EditorPlayInputSnapshot& input,
        float fixedDeltaSeconds,
        EditorPlayHudSnapshot& hud,
        EditorPlaySessionTelemetry& telemetry,
        std::string* error) {
        if (!world_) {
            if (error) *error = "preview runtime is not active";
            return false;
        }
        try {
            for (const auto& [name, pressed] : input.actions) world_->set_action_pressed(name, pressed);
            for (const auto& [name, value] : input.axes) world_->set_axis(name, value);
            world_->tick(fixedDeltaSeconds);

            std::vector<EditorObjectId> removed;
            for (const auto& [editorId, runtimeId] : editorToRuntime_) {
                if (!world_->has_object(runtimeId)) {
                    removed.push_back(editorId);
                    continue;
                }
                const std::optional<RigidTransform> transform = world_->transform(runtimeId);
                if (transform) (void)document.set_world_transform(editorId, *transform);
            }
            for (const EditorObjectId id : removed) {
                (void)document.remove_object(id);
                editorToRuntime_.erase(id);
            }
#ifdef DVE_HAVE_LUA
            if (scripts_) {
                hud.interactionPrompt = scripts_->hud_model().interaction_prompt();
                hud.selectedTool = scripts_->hud_model().selected_tool();
            }
#else
            (void)hud;
#endif
            telemetry.editorObjectCount = editorToRuntime_.size();
            telemetry.runtimeObjectCount = world_->object_count();
            telemetry.runtimeOnlyObjectCount = telemetry.runtimeObjectCount > telemetry.editorObjectCount
                ? telemetry.runtimeObjectCount - telemetry.editorObjectCount : 0U;
            return true;
        } catch (const std::exception& exception) {
            if (error) *error = std::string("preview runtime exception: ") + exception.what();
            return false;
        } catch (...) {
            if (error) *error = "preview runtime raised an unknown exception";
            return false;
        }
    }

    void stop() noexcept {
#ifdef DVE_HAVE_LUA
        scripts_.reset();
#endif
        world_.reset();
        editorToRuntime_.clear();
    }

private:
    LogSink logSink_;
    std::unique_ptr<GameWorld> world_;
    std::map<EditorObjectId, GameObjectId> editorToRuntime_;
#ifdef DVE_HAVE_LUA
    std::unique_ptr<GameScriptHost> scripts_;
#endif
};

bool EditorPlaySessionConfig::validate(std::string* error) const {
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    if (!finite_positive(fixedDeltaSeconds) || fixedDeltaSeconds > 0.1F)
        return fail("fixedDeltaSeconds must be finite and in (0, 0.1]");
    if (!finite_positive(maximumAccumulatedSeconds) || maximumAccumulatedSeconds > 5.0F)
        return fail("maximumAccumulatedSeconds must be finite and in (0, 5]");
    if (maximumAccumulatedSeconds < fixedDeltaSeconds)
        return fail("maximumAccumulatedSeconds must be at least one fixed step");
    if (maximumSubstepsPerUpdate == 0U || maximumSubstepsPerUpdate > 1024U)
        return fail("maximumSubstepsPerUpdate must be between 1 and 1024");
    if (!finite_positive(timeScale) || timeScale > 100.0F)
        return fail("timeScale must be finite and in (0, 100]");
    return true;
}

EditorPlaySession::EditorPlaySession() = default;
EditorPlaySession::~EditorPlaySession() { reset_runtime_state(); }

bool EditorPlaySession::start(
    EditorWorkspace& workspace,
    const EditorMaterialLibrary& materials,
    EditorMode mode,
    EditorPlaySessionConfig config,
    LogSink logSink,
    std::string* error) {
    if (active()) {
        if (error) *error = "a play-in-editor session is already active";
        return false;
    }
    if (mode != EditorMode::Simulate && mode != EditorMode::Play) {
        if (error) *error = "play-in-editor sessions must start in Simulate or Play mode";
        return false;
    }
    if (!config.validate(error)) return false;
    if (workspace.mode() != EditorMode::Edit) {
        if (error) *error = "workspace must be in Edit mode before starting a session";
        return false;
    }

    std::string modeError;
    if (!workspace.set_mode(mode, &modeError)) {
        if (error) *error = std::move(modeError);
        return false;
    }

    config_ = std::move(config);
    logSink_ = std::move(logSink);
    mode_ = mode;
    state_ = EditorPlaySessionState::Running;
    telemetry_ = {};
    telemetry_.state = state_;
    telemetry_.mode = mode_;
    accumulatorSeconds_ = 0.0;
    runtime_ = std::make_unique<Runtime>(logSink_);
    std::string runtimeError;
    if (!runtime_->start(workspace.document(), materials, mode_, config_, telemetry_, &runtimeError)) {
        runtime_.reset();
        state_ = EditorPlaySessionState::Stopped;
        mode_ = EditorMode::Edit;
        std::string restoreError;
        (void)workspace.set_mode(EditorMode::Edit, &restoreError);
        if (error) *error = runtimeError.empty() ? restoreError : runtimeError;
        return false;
    }
    log(EditorLogLevel::Info, mode == EditorMode::Play
        ? "Play-in-editor session started" : "Simulation session started");
    return true;
}

bool EditorPlaySession::update(EditorWorkspace& workspace, float elapsedSeconds, std::string* error) {
    telemetry_.stepsLastUpdate = 0U;
    if (!active()) return true;
    if (workspace.mode() != mode_) {
        if (error) *error = "workspace mode changed outside the play-session controller";
        return false;
    }
    if (state_ == EditorPlaySessionState::Paused) return true;
    if (!(elapsedSeconds >= 0.0F) || !std::isfinite(elapsedSeconds)) {
        if (error) *error = "elapsedSeconds must be finite and non-negative";
        return false;
    }

    const double scaled = static_cast<double>(elapsedSeconds) * static_cast<double>(config_.timeScale);
    const double bounded = std::min(scaled, static_cast<double>(config_.maximumAccumulatedSeconds));
    telemetry_.droppedSeconds += std::max(0.0, scaled - bounded);
    accumulatorSeconds_ = std::min(
        accumulatorSeconds_ + bounded,
        static_cast<double>(config_.maximumAccumulatedSeconds));

    const double fixed = static_cast<double>(config_.fixedDeltaSeconds);
    while (accumulatorSeconds_ + 1.0e-12 >= fixed &&
           telemetry_.stepsLastUpdate < config_.maximumSubstepsPerUpdate) {
        if (!fixed_step(workspace, error)) return false;
        accumulatorSeconds_ -= fixed;
        ++telemetry_.stepsLastUpdate;
    }
    if (accumulatorSeconds_ + 1.0e-12 >= fixed) {
        const double retained = std::fmod(accumulatorSeconds_, fixed);
        telemetry_.droppedSeconds += accumulatorSeconds_ - retained;
        accumulatorSeconds_ = retained;
    }
    return true;
}

bool EditorPlaySession::pause(std::string* error) {
    if (!active()) { if (error) *error = "no active play-in-editor session"; return false; }
    state_ = EditorPlaySessionState::Paused;
    telemetry_.state = state_;
    log(EditorLogLevel::Info, "Play-in-editor session paused");
    return true;
}

bool EditorPlaySession::resume(std::string* error) {
    if (!active()) { if (error) *error = "no active play-in-editor session"; return false; }
    state_ = EditorPlaySessionState::Running;
    telemetry_.state = state_;
    log(EditorLogLevel::Info, "Play-in-editor session resumed");
    return true;
}

bool EditorPlaySession::toggle_pause(std::string* error) { return paused() ? resume(error) : pause(error); }

bool EditorPlaySession::single_step(EditorWorkspace& workspace, std::string* error) {
    if (!active()) { if (error) *error = "no active play-in-editor session"; return false; }
    if (!paused()) { if (error) *error = "single-step requires a paused session"; return false; }
    telemetry_.stepsLastUpdate = 0U;
    if (!fixed_step(workspace, error)) return false;
    telemetry_.stepsLastUpdate = 1U;
    return true;
}

bool EditorPlaySession::stop(EditorWorkspace& workspace, std::string* error) {
    if (!active()) {
        if (workspace.mode() == EditorMode::Edit) return true;
        if (error) *error = "no active play-in-editor session";
        return false;
    }
    if (runtime_) runtime_->stop();
    runtime_.reset();
    std::string restoreError;
    const bool restored = workspace.set_mode(EditorMode::Edit, &restoreError);
    if (!restored) { if (error) *error = std::move(restoreError); return false; }
    log(EditorLogLevel::Info, "Play-in-editor session stopped; edit snapshot restored");
    reset_runtime_state();
    return true;
}

bool EditorPlaySession::accept_runtime_changes(EditorWorkspace& workspace, std::string* error) {
    if (!active()) { if (error) *error = "no active play-in-editor session"; return false; }
    if (runtime_) runtime_->stop();
    runtime_.reset();
    std::string acceptError;
    if (!workspace.accept_simulation_state(&acceptError)) {
        if (error) *error = std::move(acceptError);
        return false;
    }
    log(EditorLogLevel::Warning, "Runtime transforms accepted into the editable scene");
    reset_runtime_state();
    return true;
}

void EditorPlaySession::set_action_pressed(std::string action, bool pressed) {
    if (action.empty()) return;
    input_.actions[std::move(action)] = pressed;
}

void EditorPlaySession::set_axis(std::string axis, float value) {
    if (axis.empty() || !std::isfinite(value)) return;
    input_.axes[std::move(axis)] = std::clamp(value, -1.0F, 1.0F);
}

void EditorPlaySession::clear_input() noexcept { input_.actions.clear(); input_.axes.clear(); }

bool EditorPlaySession::fixed_step(EditorWorkspace& workspace, std::string* error) {
    if (!runtime_) { if (error) *error = "preview runtime is unavailable"; return false; }
    if (!runtime_->fixed_step(workspace.document(), input_, config_.fixedDeltaSeconds, hud_, telemetry_, error))
        return false;
    ++telemetry_.fixedTickCount;
    telemetry_.simulatedSeconds += static_cast<double>(config_.fixedDeltaSeconds);
    return true;
}

void EditorPlaySession::reset_runtime_state() noexcept {
    if (runtime_) runtime_->stop();
    runtime_.reset();
    config_ = {};
    input_ = {};
    hud_ = {};
    telemetry_.state = EditorPlaySessionState::Stopped;
    telemetry_.mode = EditorMode::Edit;
    state_ = EditorPlaySessionState::Stopped;
    mode_ = EditorMode::Edit;
    accumulatorSeconds_ = 0.0;
    logSink_ = {};
}

void EditorPlaySession::log(EditorLogLevel level, std::string text) const {
    if (logSink_) logSink_(level, std::move(text));
}

std::string_view editor_play_session_state_label(EditorPlaySessionState state) noexcept {
    switch (state) {
        case EditorPlaySessionState::Stopped: return "Stopped";
        case EditorPlaySessionState::Running: return "Running";
        case EditorPlaySessionState::Paused: return "Paused";
    }
    return "Unknown";
}

} // namespace dve::editor
