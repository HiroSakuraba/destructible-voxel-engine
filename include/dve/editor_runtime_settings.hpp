#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include "dve/editor_settings.hpp"
#include "dve/editor_play_session.hpp"
#include "dve/camera_system.hpp"

namespace dve::editor {

// Resolve once at an application boundary, never inside an audio callback/draw loop.
class RuntimeSettingsReader {
public:
    explicit RuntimeSettingsReader(const EditorSettingsRegistry& registry) : registry_(registry) {}
    template<class T> [[nodiscard]] T get(std::string_view id) const {
        return std::get<T>(registry_.value(id));
    }
    [[nodiscard]] float number(std::string_view id) const { return static_cast<float>(get<double>(id)); }
    [[nodiscard]] std::uint32_t integer(std::string_view id) const {
        return static_cast<std::uint32_t>(get<std::int64_t>(id));
    }
private:
    const EditorSettingsRegistry& registry_;
};

inline EditorPlaySessionConfig play_session_settings(const EditorSettingsRegistry& registry) {
    const RuntimeSettingsReader s(registry);
    EditorPlaySessionConfig config;
    config.fixedDeltaSeconds = s.number("physics.fixed_timestep");
    config.maximumSubstepsPerUpdate = s.integer("physics.max_substeps");
    config.runStartupScriptInPlay = s.get<bool>("scripting.lua");
    const auto backend = s.get<std::string>("physics.backend");
    config.physicsBackend = backend == "jolt" ? Physics3DBackend::Jolt
        : backend == "box3d" ? Physics3DBackend::Box3D
        : backend == "reference" ? Physics3DBackend::Reference : Physics3DBackend::Automatic;
    config.physicsWorld.gravity = {0.0F, s.number("physics.gravity"), 0.0F};
    config.physicsWorld.workerThreads = s.integer("physics.worker_threads");
    config.physicsWorld.subStepCount = s.integer("physics.substeps");
    config.physicsWorld.velocityIterations = s.integer("physics.velocity_iterations");
    config.physicsWorld.positionIterations = s.integer("physics.position_iterations");
    config.physicsWorld.deterministicSimulation = s.get<bool>("physics.deterministic");
    config.physicsWorld.allowSleeping = s.get<bool>("physics.allow_sleeping");
    config.physicsWorld.continuousCollision = s.get<bool>("physics.continuous_collision");
    return config;
}

inline void configure_camera_rig(camera::CameraRig& rig, const EditorSettingsRegistry& registry,
                                 bool defaults = true) {
    const RuntimeSettingsReader s(registry);
    const auto selected = [&](std::string_view id) {
        return defaults || registry.has_override(SettingScope::User, id) ||
            registry.has_override(SettingScope::Project, id) || registry.has_override(SettingScope::Session, id);
    };
    if (selected("camera.position_damping")) rig.framing.positionDampingSeconds = s.number("camera.position_damping");
    if (selected("camera.aim_damping")) rig.framing.aimDampingSeconds = s.number("camera.aim_damping");
    if (selected("camera.look_ahead")) rig.framing.lookAheadSeconds = s.number("camera.look_ahead");
    if (selected("camera.dead_zone")) rig.framing.deadZoneFraction = s.number("camera.dead_zone");
    if (selected("camera.soft_zone")) rig.framing.softZoneFraction = s.number("camera.soft_zone");
    if (selected("camera.collision")) rig.collision.enabled = s.get<bool>("camera.collision");
    if (selected("camera.collision_radius")) rig.collision.probeRadiusMeters = s.number("camera.collision_radius");
    if (selected("camera.collision_recovery")) rig.collision.recoverySeconds = s.number("camera.collision_recovery");
    if (selected("camera.preserve_line_of_sight")) rig.collision.preserveLineOfSight = s.get<bool>("camera.preserve_line_of_sight");
}

// Round up: integer-ms host waits must never present earlier than a requested cap.
inline std::chrono::milliseconds editor_frame_floor(std::int64_t frameLimit) noexcept {
    return std::chrono::milliseconds(frameLimit > 0 ? std::max<std::int64_t>(4, (1000 + frameLimit - 1) / frameLimit) : 4);
}

inline float controller_axis_with_dead_zone(float value, float deadZone) noexcept {
    if (!std::isfinite(value)) return 0.0F;
    const float magnitude = std::clamp(std::fabs(value), 0.0F, 1.0F);
    if (magnitude <= deadZone) return 0.0F;
    return std::copysign((magnitude - deadZone) / (1.0F - deadZone), value);
}
inline std::array<float, 2> controller_stick_with_dead_zone(float x, float y, float deadZone) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    const float magnitude = std::hypot(x, y);
    if (magnitude <= deadZone || magnitude <= 0.0F) return {};
    const float scaled = (std::min(magnitude, 1.0F) - deadZone) / (1.0F - deadZone);
    return {x / magnitude * scaled, y / magnitude * scaled};
}
} // namespace dve::editor
