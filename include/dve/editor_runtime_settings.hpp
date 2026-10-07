#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include "dve/editor_settings.hpp"
#include "dve/editor_play_session.hpp"
#include "dve/camera_system.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/render_environment.hpp"

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

// Applies the Rendering settings to a render environment. Exposure is a
// linear pre-tonemap multiplier (1 is neutral, 2 is one stop up) and replaces
// the environment value outright, so a global and an authored exposure never
// multiply twice in this path. Shadow quality selects the directional sample
// count from the published preset table (low 1, medium 2, high 4, ultra 8);
// shadow strength and the sun's angular radius map to the same-named fields.
// GI quality likewise selects the one-bounce sample count (low 1, medium 4,
// high 8, ultra 16) and never touches intensity or distance. The two boolean
// switches encode off through the fields they gate: bloom off zeroes the
// composite intensity (the extraction threshold stays as authored), and
// subsurface off zeroes the maximum transmission distance, which leaves the
// ordinary surface model because a zero-length search transmits nothing.
inline RenderEnvironment render_environment_settings(const EditorSettingsRegistry& registry,
                                                     RenderEnvironment environment = {}) {
    const RuntimeSettingsReader s(registry);
    environment.exposure = s.number("render.exposure");
    const auto tonemap = s.get<std::string>("render.tonemap");
    environment.tonemapOperator = tonemap == "reinhard" ? TonemapOperator::Reinhard
        : tonemap == "clamp" ? TonemapOperator::Clamp : TonemapOperator::ACES;
    const auto shadowMode = s.get<std::string>("render.shadow_mode");
    environment.shadowMode = shadowMode == "off" ? ShadowMode::Off
        : shadowMode == "hard" ? ShadowMode::Hard
        : shadowMode == "contact" ? ShadowMode::Contact
        : shadowMode == "hybrid" ? ShadowMode::Hybrid : ShadowMode::Soft;
    environment.shadowStrength = s.number("render.shadow_strength");
    environment.shadowSoftnessRadians = s.number("render.shadow_softness");
    const auto quality = s.get<std::string>("render.shadow_quality");
    environment.shadowSamples = quality == "low" ? 1U
        : quality == "medium" ? 2U
        : quality == "ultra" ? 8U : 4U;
    environment.contactShadowDistanceMeters = s.number("render.contact_shadow_distance");
    const auto giMode = s.get<std::string>("render.gi_mode");
    environment.globalIlluminationMode = giMode == "off" ? GlobalIlluminationMode::Off
        : giMode == "ambient" ? GlobalIlluminationMode::AmbientHemisphere
        : GlobalIlluminationMode::VoxelOneBounce;
    const auto giQuality = s.get<std::string>("render.gi_quality");
    environment.globalIlluminationSamples = giQuality == "low" ? 1U
        : giQuality == "high" ? 8U
        : giQuality == "ultra" ? 16U : 4U;
    environment.globalIlluminationIntensity = s.number("render.gi_intensity");
    environment.globalIlluminationMaxDistanceMeters = s.number("render.gi_distance");
    environment.bloomThreshold = s.number("render.bloom_threshold");
    if (!s.get<bool>("render.bloom")) environment.bloomIntensity = 0.0F;
    if (!s.get<bool>("material.subsurface")) environment.subsurfaceMaxDistanceMeters = 0.0F;
    return environment;
}

// Resolves render.texture_filter / render.anisotropy into the residency's
// sampler policy. The anisotropy level is a request: the residency clamps it
// to the device limit and reports the effective level.
inline render::MaterialSamplerPolicy material_sampler_policy(const EditorSettingsRegistry& registry) {
    const RuntimeSettingsReader s(registry);
    render::MaterialSamplerPolicy policy;
    policy.overrideFiltering = true;
    const auto filter = s.get<std::string>("render.texture_filter");
    if (filter == "nearest") {
        policy.minFilter = rhi::FilterMode::Nearest;
        policy.magFilter = rhi::FilterMode::Nearest;
        policy.mipmapFilter = rhi::MipmapFilterMode::Nearest;
    } else if (filter == "bilinear") {
        policy.minFilter = rhi::FilterMode::Linear;
        policy.magFilter = rhi::FilterMode::Linear;
        policy.mipmapFilter = rhi::MipmapFilterMode::Nearest;
    } else if (filter == "anisotropic") {
        policy.minFilter = rhi::FilterMode::Linear;
        policy.magFilter = rhi::FilterMode::Linear;
        policy.mipmapFilter = rhi::MipmapFilterMode::Linear;
        policy.anisotropy = true;
        policy.requestedMaxAnisotropy = static_cast<float>(s.integer("render.anisotropy"));
    } else {  // trilinear, the registry default
        policy.minFilter = rhi::FilterMode::Linear;
        policy.magFilter = rhi::FilterMode::Linear;
        policy.mipmapFilter = rhi::MipmapFilterMode::Linear;
    }
    return policy;
}

// Resolves voxel.debris_limit: the maximum number of active debris bodies
// (damage-split fragments) a game world keeps. Play sessions apply it to
// the world at start; zero suppresses debris creation.
inline std::size_t debris_limit(const EditorSettingsRegistry& registry) {
    const auto value = RuntimeSettingsReader(registry).get<std::int64_t>("voxel.debris_limit");
    return value > 0 ? static_cast<std::size_t>(value) : 0U;
}

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
    config.environment = render_environment_settings(registry);
    config.debrisLimit = debris_limit(registry);
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
// Resolves diagnostics.gpu_markers for renderer creation: when true, the
// live renderer wraps its frame sections in named RHI debug labels. The
// Vulkan backend reports DeviceCapabilities::debugLabels only when
// VK_EXT_debug_utils is actually present; on other devices the labels
// remain validated bookkeeping, never a claimed capture feature.
inline bool render_debug_labels(const EditorSettingsRegistry& registry) {
    return RuntimeSettingsReader(registry).get<bool>("diagnostics.gpu_markers");
}

// scripting.strict_errors as a validation policy. Strict blocks scene
// saves and builds on script compile errors; lenient proceeds but keeps a
// visible diagnostic in the editor log. Script error reporting itself is
// never disabled by this policy.
enum class ScriptErrorPolicy : std::uint8_t { Block, Warn };

inline ScriptErrorPolicy script_error_policy(const EditorSettingsRegistry& registry) {
    return RuntimeSettingsReader(registry).get<bool>("scripting.strict_errors")
        ? ScriptErrorPolicy::Block : ScriptErrorPolicy::Warn;
}

} // namespace dve::editor
