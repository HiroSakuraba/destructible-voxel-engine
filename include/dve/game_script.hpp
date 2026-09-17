#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dve/game_ui.hpp"
#include "dve/game_world.hpp"
#include "dve/master_material.hpp"
#include "dve/render_environment.hpp"

namespace dve {

// Embeds Lua 5.4 and binds a `world` table to the given GameWorld's operations: object
// lookup/spawn/destroy, transform get/set, impulse/force, damage, raycast, timers, and the
// on_tick/on_damage/on_destroyed events. This is the scripting layer proper; GameWorld itself
// has no knowledge of Lua (see its header), so this class is the only thing that needs to
// change if the scripting language ever changes.
//
// The `world` table, as seen from a script (every 3D vector in or out is a {x=,y=,z=} table;
// ids are plain numbers; everything that can fail returns nil rather than raising, except
// spawn_box/schedule_*/on_* misuse, which raise a Lua error via luaL_error the normal way):
//   world.find_by_name(name) -> id or nil
//   world.find_by_tag(tag) -> array table of ids
//   world.find_by_group(group) / world.find_by_layer(layer) -> array table of enabled ids
//   world.is_enabled(id) / world.set_enabled(id, bool) -> bool
//   world.find_by_component(type) -> array table of ids
//   world.get_components(id) -> array of {id,type,enabled,properties} tables or nil
//   world.get_component(id, componentId) -> {id,type,enabled,properties} or nil
//   world.add_component(id, type[, propertiesTable, enabled]) -> componentId or nil,errorString
//   world.set_component_property(id, componentId, property, value) -> bool or false,errorString
//   world.remove_component(id, componentId) -> bool
//   world.set_component_enabled(id, componentId, bool) -> bool
//   world.get_position(id) -> {x,y,z} or nil
//   world.set_position(id, x, y, z) -> bool
//   world.get_rotation(id) -> {x,y,z} degrees (Euler XYZ) or nil
//   world.set_rotation(id, degreesX, degreesY, degreesZ) -> bool
//   world.get_velocity(id) -> {x,y,z} or nil (nil for markers and static bodies)
//   world.set_velocity(id, x, y, z) -> bool
//   world.apply_impulse(id, x, y, z) -> bool
//   world.apply_force(id, x, y, z) -> bool
//   world.damage_sphere(id, x, y, z, radius) -> removed_voxel_count or nil
//   world.raycast(ox,oy,oz, dx,dy,dz, maxDistance) -> {id, position={x,y,z}, normal={x,y,z}, distance, material} or nil
//   world.sphere_overlap(x,y,z, radius) -> array table of ids (exact oriented-box test, not cached)
//   world.spawn_box(name, sizeVoxels, voxelSizeMeters, x,y,z, dynamic, density) -> id (or nil, errorString)
//   world.spawn_box3(name, sizeX,sizeY,sizeZ, voxelSizeMeters, x,y,z, dynamic, density) -> id (or nil, errorString)
//   world.spawn_asset(path, x,y,z, dynamic, structural) -> id (or nil, errorString); loads a .dvox or .dmesh
//     cooked asset and uses its own real per-material densities, not a single uniform density
//   world.destroy(id) -> bool
//   world.pool_create_box(name, capacity, sizeX,sizeY,sizeZ, voxelSize, dynamic, density) -> poolId or nil,error
//   world.pool_acquire(poolId, x,y,z) -> id or nil,error
//   world.pool_release(id) -> bool or false,error; world.pool_available(poolId) -> integer
//   world.character_add(pawn[, configTable]) -> bool or false,errorString
//   world.character_set_input(pawn, moveX, moveY, jumpPressed, crouchHeld) -> bool
//   world.character_get(pawn) -> state table or nil
//   world.player_create(name[, local]) -> playerId
//   world.player_possess(playerId, pawn) -> bool or false,errorString
//   world.player_set_input(playerId, moveX, moveY, jumpPressed, crouchHeld) -> bool
//   world.trigger_create_box(name, x,y,z, halfX,halfY,halfZ, oneShot[, requiredTag]) -> triggerId
//   world.trigger_create_sphere(name, x,y,z, radius, oneShot[, requiredTag]) -> triggerId
//   world.on_trigger(function(triggerId, kind, objectId, fixedTick) ... end)
//   world.schedule_once(seconds, fn) -> timerId
//   world.schedule_repeating(intervalSeconds, fn) -> timerId
//   world.cancel_timer(timerId) -> bool
//   world.on_tick(function(dt) ... end)
//   world.on_damage(function(objectId, center, radius, removedCount, destroyed, fragmentIds) ... end)
//   world.on_destroyed(function(objectId) ... end)
//   world.on_lifecycle(function(kind, objectId, otherId, componentId, componentType, poolName) ... end)
//   world.set_global(key, number) -> true or false,errorString
//     mirrors canonical or explicitly material-bound names into the Material Parameter Collection;
//     unrelated gameplay globals remain in the ordinary script dictionary
//   world.get_global(key) -> number or nil
//   world.set_global_vector(key, x,y,z[,w]) -> true or false,errorString
//   world.get_global_vector(key) -> {x,y,z,w} or nil
//   world.hud_set_interaction_prompt(text)
//   world.hud_set_tools({ {id=,label=,enabled=}, ... })
//   world.hud_select_next_tool() -> bool
//   world.hud_selected_tool() -> {id,label,enabled} or nil
//   world.is_action_pressed(name) -> bool / world.set_action_pressed(name, bool)
//   world.get_axis(name) -> number / world.set_axis(name, number)
//   world.log(message)
//
//   -- Master materials (see master_material.hpp for the full design rationale: this engine
//   -- has one shading model per voxel, not a per-material shader graph, so parameters reach
//   -- VoxelMaterialDefinition through a small reserved-name set, not arbitrary graph wiring)
//   world.create_master_material(name, {
//       shading_model = "standard_pbr" | "unlit" | "emissive" | "subsurface" |
//                       "two_sided_foliage" | "clear_coat",
//       blend_mode = "opaque" | "masked" | "translucent",       -- optional, default opaque
//       scalars = { {name=, default=, min=, max=}, ... },
//       vectors = { {name=, default={x,y,z,w}}, ... },
//       switches = { {name=, default=}, ... },
//       global_scalars = { {parameter=, global=, combine="replace"|"add"|"multiply"}, ... },
//       global_vectors = { {parameter=, global=, combine=...}, ... },
//   }) -> masterId (or nil, errorString)
//   world.create_material_instance({
//       name=, material_id=, master=, parent=,      -- parent is optional (instance chaining)
//       scalars = { Roughness=0.6, ... },            -- name-keyed, not array-of-tables
//       vectors = { BaseColor={0.5,0.3,0.1,1.0}, ... },
//       layers = { {material_id=, weight=, blend="lerp"|"multiply"|"additive", enabled=}, ... },
//       density=, structural_strength=, fracture_resistance=, flammability=,
//       thermal_conductivity=, structural=,          -- all optional, physics fields
//   }) -> instanceId (or nil, errorString)
//   world.set_material_parameter(materialId, name, numberOrVectorTable) -> bool (or false, errorString)
//   world.get_material_parameter(materialId, name) -> number or {x,y,z,w} table or nil
//   world.clear_material_parameters(materialId) -> bool (had any runtime overrides to clear)
//   world.set_material_layer_weight(materialId, oneBasedLayerIndex, weight) -> bool or false,errorString
//   world.get_material_layer_weight(materialId, oneBasedLayerIndex) -> number or nil
//
//   -- Render environment (the Material-Parameter-Collection / Godot-Environment-resource
//   -- equivalent: lighting, exposure, tonemap, and bloom settings in one place, reserved
//   -- names like the material parameter functions above)
//   world.set_environment_scalar(name, number) -> bool (or false, errorString)
//     names: SunIntensity, SubsurfaceMaxDistanceMeters, Exposure, BloomThreshold, BloomIntensity, BloomRadius,
//            GlobalIlluminationIntensity, GlobalIlluminationMaxDistanceMeters, GlobalIlluminationSamples,
//            ShadowStrength, ShadowSoftnessRadians, ShadowSamples, ShadowMaxDistanceMeters,
//            ContactShadowDistanceMeters, ShadowBiasMeters
//   world.set_environment_vector(name, x, y, z) -> bool (or false, errorString)
//     names: SunDirection, SunColor, SkyColor, GroundColor, GlobalTint
//   world.set_environment_tonemap_operator("aces" | "reinhard" | "clamp") -> bool (or false, errorString)
//   world.set_environment_gi_mode("off" | "ambient" | "voxel_one_bounce") -> bool
//   world.set_environment_shadow_mode("off" | "hard" | "soft" | "contact" | "hybrid") -> bool
//   world.get_environment() -> table snapshot of every current environment value
//
//   -- Runtime camera and cinematic-sequence control
//   world.camera_force_live(viewportId, rigId[, cut]) -> bool
//   world.camera_clear_forced_live(viewportId[, cut]) -> bool
//   world.camera_set_state(viewportId, stateName) -> bool
//   world.camera_start_shake(viewportId, shakeId, positionMeters, rotationDegrees,
//                            durationSeconds[, frequencyHertz]) -> true or false,errorString
//   world.camera_stop_shake(viewportId, shakeId) -> bool
//   world.camera_load_sequence(viewportId, path) -> true or false,errorString
//   world.camera_play_sequence(viewportId[, loop]) -> bool
//   world.camera_pause_sequence(viewportId) / stop / seek(viewportId, seconds) -> bool
//   world.camera_set_accessibility("standard"|"reduced_motion"|"photosensitive",
//                                  horizonLock[, motionScale, shakeScale, bloomScale, maxDofWeight])
//       -> true or false,errorString
//   world.camera_get_accessibility() -> table
//   world.camera_get(viewportId) -> pose, shot, marker, events, and collision telemetry table or nil
//   world.camera_save_runtime_state(path) / camera_load_runtime_state(path)
//       -> true or false,errorString
//
// One host per GameWorld, for the lifetime of that world: the constructor registers C++
// listeners with the world (on_tick etc.) that dispatch into whatever Lua functions scripts
// have registered via world.on_tick(fn) and friends, so a GameScriptHost must outlive any
// script code it has run that registered such a listener.
class GameScriptHost {
public:
    using LogSink = std::function<void(bool, std::string)>;
    explicit GameScriptHost(GameWorld& world);
    ~GameScriptHost();
    GameScriptHost(const GameScriptHost&) = delete;
    GameScriptHost& operator=(const GameScriptHost&) = delete;

    // Runs a chunk of Lua source immediately (typically: define some functions, call
    // world.on_tick/on_damage/on_destroyed to register them, maybe world.find_by_name/spawn
    // to set up initial state). Returns false and fills *error on a syntax or runtime error;
    // the world is left exactly as it was up to the point of the error (any object
    // creation/damage/etc. calls made before the error already happened, same as a partially-
    // run script in any embedded scripting language).
    [[nodiscard]] bool run_string(const std::string& code, const std::string& chunkName, std::string* error = nullptr);
    [[nodiscard]] bool run_file(const std::filesystem::path& path, std::string* error = nullptr);
    void set_log_sink(LogSink sink);

    // Number of Lua functions currently registered via world.on_tick/on_damage/on_destroyed.
    // Exposed mainly for tests to confirm registration actually happened.
    [[nodiscard]] std::size_t tick_listener_count() const noexcept;
    [[nodiscard]] std::size_t damage_listener_count() const noexcept;
    [[nodiscard]] std::size_t destroyed_listener_count() const noexcept;

    // A small persistent key/value store scripts can read and write via world.set_global(key,
    // value) / world.get_global(key), and embedding C++ code can read via this accessor
    // (mainly useful for tests and for a host application polling script-side state without
    // its own dedicated binding for it). Numbers only, not a general save-game system.
    [[nodiscard]] std::optional<double> global_number(const std::string& key) const;
    [[nodiscard]] std::optional<Float4> global_vector(const std::string& key) const;

    // HUD state a script has set via world.hud_set_interaction_prompt/set_tools/
    // select_next_tool. A host application's own rendering code reads this to know what to
    // draw; nothing in this codebase renders it (see the notes doc for what "HUD binding"
    // does and does not include).
    [[nodiscard]] const ui::GameHudModel& hud_model() const;

    // Lighting/exposure/tonemap/bloom settings a script has adjusted via
    // world.set_environment_scalar/set_environment_vector/set_environment_tonemap_operator -
    // the Material-Parameter-Collection/Godot-Environment-resource equivalent: one place
    // global rendering state lives, script-adjustable by name, rather than scattered
    // per-shader constants. A host's rendering code would pack this via
    // gpu_render_environment.hpp for upload; nothing in this codebase's GPU pipeline consumes
    // it yet (no shading pass runs on real hardware), same caveat as hud_model() above.
    [[nodiscard]] const RenderEnvironment& environment() const;

    // Masters and instances a script has defined via world.create_master_material/
    // create_material_instance, plus any runtime parameter overrides from
    // world.set_material_parameter. A host's own rendering code would read
    // material_library().resolved(materialId) to get the flat, resolved VoxelMaterialDefinition
    // for a voxel's material id; nothing in this codebase's GPU pipeline consumes it yet (no
    // shading pass exists), same caveat as hud_model() above.
    [[nodiscard]] const MaterialLibrary& material_library() const;
    [[nodiscard]] const MaterialParameterCollection& material_parameter_collection() const;

    // Public only so the free C functions bound into the `world` Lua table (which must be
    // plain function pointers matching lua_CFunction, not members) can reach it from within
    // game_script.cpp; the definition itself stays in the .cpp, so this header still never
    // includes lua.h or exposes lua_State to anything that includes this file.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace dve
