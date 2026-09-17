#include "dve/game_script.hpp"

#include "dve/game_ui.hpp"
#include "dve/gameplay_runtime.hpp"
#include "dve/master_material.hpp"
#include "dve/render_environment.hpp"
#include "dve/camera_runtime.hpp"
#include "dve/camera_sequence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

extern "C" {
#include <lua5.4/lauxlib.h>
#include <lua5.4/lua.h>
#include <lua5.4/lualib.h>
}

namespace dve {

struct GameScriptHost::Impl {
    lua_State* L{nullptr};
    GameWorld* world{nullptr};
    std::vector<int> tickRefs;
    std::vector<int> damageRefs;
    std::vector<int> destroyRefs;
    std::vector<int> triggerRefs;
    std::vector<int> lifecycleRefs;
    std::unordered_map<std::string, double> globals;
    std::unordered_map<std::string, Float4> globalVectors;
    // Owned here, not by GameWorld: HUD state is what-to-display, not simulation state, and a
    // gameplay script is the natural thing to drive it ("show an interaction prompt near a
    // door"). A host application would read this back through whatever it uses to render UI;
    // no rendering of it happens anywhere in this codebase yet (see the notes doc).
    ui::GameHudModel hud;
    MaterialLibrary materials;
    RenderEnvironment environment;
    LogSink logSink;
};

namespace {

// The registered Lua function refs live in the Impl this global-ish registry key points at;
// every bound C function reaches the world/impl through the Lua state itself (not a C++
// global), so multiple independent GameScriptHost/lua_State pairs remain possible later even
// though nothing in this codebase creates more than one today.
GameScriptHost::Impl* impl_from_state(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "__dve_host_impl");
    auto* impl = static_cast<GameScriptHost::Impl*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return impl;
}
GameWorld& world_from_state(lua_State* L) { return *impl_from_state(L)->world; }

const char* lifecycle_kind_name(GameLifecycleEventKind kind) noexcept {
    switch (kind) {
    case GameLifecycleEventKind::Spawn: return "spawn";
    case GameLifecycleEventKind::Enable: return "enable";
    case GameLifecycleEventKind::Disable: return "disable";
    case GameLifecycleEventKind::OverlapBegin: return "overlap_begin";
    case GameLifecycleEventKind::OverlapEnd: return "overlap_end";
    case GameLifecycleEventKind::Destroy: return "destroy";
    case GameLifecycleEventKind::ComponentAdded: return "component_added";
    case GameLifecycleEventKind::ComponentRemoved: return "component_removed";
    case GameLifecycleEventKind::ComponentEnabled: return "component_enabled";
    case GameLifecycleEventKind::ComponentDisabled: return "component_disabled";
    case GameLifecycleEventKind::PoolAcquire: return "pool_acquire";
    case GameLifecycleEventKind::PoolRelease: return "pool_release";
    }
    return "unknown";
}

[[nodiscard]] Float3 check_float3(lua_State* L, int startIndex) {
    return Float3{
        static_cast<float>(luaL_checknumber(L, startIndex)),
        static_cast<float>(luaL_checknumber(L, startIndex + 1)),
        static_cast<float>(luaL_checknumber(L, startIndex + 2))};
}

// Pushes exactly one value: a new table {x=,y=,z=}. Stack-balanced on its own, so a caller
// can immediately lua_setfield() it into a parent table or leave it as a return value.
void push_float3_table(lua_State* L, Float3 value) {
    lua_newtable(L);
    lua_pushnumber(L, value.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, value.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, value.z); lua_setfield(L, -2, "z");
}

void push_float4_table(lua_State* L, Float4 value) {
    lua_newtable(L);
    lua_pushnumber(L, value.x); lua_setfield(L, -2, "x"); lua_pushnumber(L, value.x); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, value.y); lua_setfield(L, -2, "y"); lua_pushnumber(L, value.y); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, value.z); lua_setfield(L, -2, "z"); lua_pushnumber(L, value.z); lua_rawseti(L, -2, 3);
    lua_pushnumber(L, value.w); lua_setfield(L, -2, "w"); lua_pushnumber(L, value.w); lua_rawseti(L, -2, 4);
}


void push_component_value(lua_State* L, const ComponentValue& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) lua_pushboolean(L, item);
        else if constexpr (std::is_same_v<T, std::int64_t>) lua_pushinteger(L, static_cast<lua_Integer>(item));
        else if constexpr (std::is_same_v<T, double>) lua_pushnumber(L, item);
        else if constexpr (std::is_same_v<T, std::string>) lua_pushlstring(L, item.data(), item.size());
        else if constexpr (std::is_same_v<T, Float3>) push_float3_table(L, item);
        else if constexpr (std::is_same_v<T, Quaternion>) {
            lua_newtable(L);
            lua_pushnumber(L, item.x); lua_setfield(L, -2, "x");
            lua_pushnumber(L, item.y); lua_setfield(L, -2, "y");
            lua_pushnumber(L, item.z); lua_setfield(L, -2, "z");
            lua_pushnumber(L, item.w); lua_setfield(L, -2, "w");
        }
    }, value);
}

std::optional<ComponentValue> component_value_from_lua(lua_State* L, int index) {
    index = lua_absindex(L, index);
    if (lua_isboolean(L, index)) return ComponentValue{lua_toboolean(L, index) != 0};
    if (lua_isinteger(L, index)) return ComponentValue{static_cast<std::int64_t>(lua_tointeger(L, index))};
    if (lua_isnumber(L, index)) return ComponentValue{static_cast<double>(lua_tonumber(L, index))};
    if (lua_isstring(L, index)) {
        std::size_t size{};
        const char* text = lua_tolstring(L, index, &size);
        return ComponentValue{std::string(text ? text : "", size)};
    }
    if (!lua_istable(L, index)) return std::nullopt;
    const auto field = [&](const char* name) -> std::optional<float> {
        lua_getfield(L, index, name);
        std::optional<float> result;
        if (lua_isnumber(L, -1)) result = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return result;
    };
    const auto x = field("x");
    const auto y = field("y");
    const auto z = field("z");
    if (!x || !y || !z) return std::nullopt;
    const auto w = field("w");
    if (w) return ComponentValue{Quaternion{*x, *y, *z, *w}};
    return ComponentValue{Float3{*x, *y, *z}};
}

void push_component_table(lua_State* L, const Component& component) {
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(component.id)); lua_setfield(L, -2, "id");
    lua_pushlstring(L, component.type.data(), component.type.size()); lua_setfield(L, -2, "type");
    lua_pushboolean(L, component.enabled); lua_setfield(L, -2, "enabled");
    lua_newtable(L);
    for (const auto& [name, value] : component.properties) {
        push_component_value(L, value);
        lua_setfield(L, -2, name.c_str());
    }
    lua_setfield(L, -2, "properties");
}

void report_callback_error(lua_State* L, const char* what) {
    const char* message = lua_tostring(L, -1);
    GameScriptHost::Impl* impl = impl_from_state(L);
    if (impl && impl->logSink) impl->logSink(true, std::string(what) + ": " + (message ? message : "unknown Lua error"));
    else std::fprintf(stderr, "[lua %s error] %s\n", what, message ? message : "unknown Lua error");
    lua_pop(L, 1);
}

int l_find_by_name(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto id = world_from_state(L).find_by_name(name);
    if (!id) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, static_cast<lua_Integer>(*id));
    return 1;
}

int l_find_by_tag(lua_State* L) {
    const char* tag = luaL_checkstring(L, 1);
    const auto ids = world_from_state(L).find_by_tag(tag);
    lua_newtable(L);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(ids[i]));
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int l_find_by_group(lua_State* L) {
    const char* group = luaL_checkstring(L, 1);
    const auto ids = world_from_state(L).find_by_group(group);
    lua_newtable(L);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(ids[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1U));
    }
    return 1;
}

int l_find_by_layer(lua_State* L) {
    const auto layer = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
    const auto ids = world_from_state(L).find_by_layer(layer);
    lua_newtable(L);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(ids[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1U));
    }
    return 1;
}

int l_is_enabled(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, world_from_state(L).is_enabled(id));
    return 1;
}

int l_set_enabled(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, world_from_state(L).set_enabled(id, lua_toboolean(L, 2) != 0));
    return 1;
}


int l_find_by_component(lua_State* L) {
    const char* type = luaL_checkstring(L, 1);
    const auto ids = world_from_state(L).find_by_component(type);
    lua_newtable(L);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(ids[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1U));
    }
    return 1;
}

int l_get_components(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto* components = world_from_state(L).components(id);
    if (!components) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    for (std::size_t i = 0; i < components->size(); ++i) {
        push_component_table(L, (*components)[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1U));
    }
    return 1;
}

int l_get_component(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto componentId = static_cast<ComponentId>(luaL_checkinteger(L, 2));
    const Component* component = world_from_state(L).component(id, componentId);
    if (!component) { lua_pushnil(L); return 1; }
    push_component_table(L, *component);
    return 1;
}

int l_add_component(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    Component component;
    component.type = luaL_checkstring(L, 2);
    if (lua_istable(L, 3)) {
        const int properties = lua_absindex(L, 3);
        lua_pushnil(L);
        while (lua_next(L, properties) != 0) {
            if (!lua_isstring(L, -2)) return luaL_error(L, "component property keys must be strings");
            std::size_t keySize{};
            const char* key = lua_tolstring(L, -2, &keySize);
            auto value = component_value_from_lua(L, -1);
            if (!value) return luaL_error(L, "unsupported component property value");
            component.properties.emplace(std::string(key, keySize), std::move(*value));
            lua_pop(L, 1);
        }
    }
    if (!lua_isnoneornil(L, 4)) component.enabled = lua_toboolean(L, 4) != 0;
    std::string error;
    Component* added = world_from_state(L).add_component(id, std::move(component), &error);
    if (!added) {
        lua_pushnil(L);
        lua_pushlstring(L, error.data(), error.size());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(added->id));
    return 1;
}

int l_set_component_property(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto componentId = static_cast<ComponentId>(luaL_checkinteger(L, 2));
    const char* property = luaL_checkstring(L, 3);
    auto value = component_value_from_lua(L, 4);
    if (!value) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "unsupported component property value");
        return 2;
    }
    std::string error;
    const bool success = world_from_state(L).set_component_property(
        id, componentId, property, std::move(*value), &error);
    lua_pushboolean(L, success);
    if (!success) { lua_pushlstring(L, error.data(), error.size()); return 2; }
    return 1;
}

int l_remove_component(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto componentId = static_cast<ComponentId>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, world_from_state(L).remove_component(id, componentId));
    return 1;
}

int l_set_component_enabled(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto componentId = static_cast<ComponentId>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, world_from_state(L).set_component_enabled(
        id, componentId, lua_toboolean(L, 3) != 0));
    return 1;
}

int l_get_position(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto pos = world_from_state(L).position(id);
    if (!pos) { lua_pushnil(L); return 1; }
    push_float3_table(L, *pos);
    return 1;
}

int l_set_position(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const Float3 pos = check_float3(L, 2);
    lua_pushboolean(L, world_from_state(L).set_position(id, pos));
    return 1;
}

// Rotation is exposed to Lua as Euler angles in degrees (matching the editor's own inspector
// convention), converted to/from the quaternion GameWorld actually works in, rather than
// asking script authors to construct quaternions by hand.
int l_get_rotation(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto t = world_from_state(L).transform(id);
    if (!t) { lua_pushnil(L); return 1; }
    constexpr float kDegreesPerRadian = 57.29577951308232F;
    push_float3_table(L, multiply(quaternion_to_euler_xyz(t->rotation), kDegreesPerRadian));
    return 1;
}

int l_set_rotation(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    constexpr float kRadiansPerDegree = 0.017453292519943295F;
    const Float3 degrees = check_float3(L, 2);
    const Quaternion rotation = quaternion_from_euler_xyz(multiply(degrees, kRadiansPerDegree));
    lua_pushboolean(L, world_from_state(L).set_rotation(id, rotation));
    return 1;
}

int l_get_velocity(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto v = world_from_state(L).linear_velocity(id);
    if (!v) { lua_pushnil(L); return 1; }
    push_float3_table(L, *v);
    return 1;
}

int l_set_velocity(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const Float3 v = check_float3(L, 2);
    lua_pushboolean(L, world_from_state(L).set_linear_velocity(id, v));
    return 1;
}

int l_apply_impulse(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const Float3 impulse = check_float3(L, 2);
    lua_pushboolean(L, world_from_state(L).apply_impulse(id, impulse));
    return 1;
}

int l_apply_force(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const Float3 force = check_float3(L, 2);
    lua_pushboolean(L, world_from_state(L).apply_force(id, force));
    return 1;
}

int l_damage_sphere(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const Float3 center = check_float3(L, 2);
    const float radius = static_cast<float>(luaL_checknumber(L, 5));
    const auto removed = world_from_state(L).damage_sphere(id, center, radius);
    if (!removed) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, static_cast<lua_Integer>(*removed));
    return 1;
}

int l_raycast(lua_State* L) {
    const Float3 origin = check_float3(L, 1);
    const Float3 direction = check_float3(L, 4);
    const float maxDistance = static_cast<float>(luaL_checknumber(L, 7));
    const auto hit = world_from_state(L).raycast(origin, direction, maxDistance);
    if (!hit) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(hit->objectId)); lua_setfield(L, -2, "id");
    push_float3_table(L, hit->worldPosition); lua_setfield(L, -2, "position");
    push_float3_table(L, hit->worldNormal); lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit->distance); lua_setfield(L, -2, "distance");
    lua_pushinteger(L, hit->material); lua_setfield(L, -2, "material");
    return 1;
}

int l_sphere_overlap(lua_State* L) {
    const Float3 center = check_float3(L, 1);
    const float radius = static_cast<float>(luaL_checknumber(L, 4));
    const auto ids = world_from_state(L).sphere_overlap(center, radius);
    lua_newtable(L);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(ids[i]));
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int l_is_action_pressed(lua_State* L) {
    lua_pushboolean(L, world_from_state(L).is_action_pressed(luaL_checkstring(L, 1)));
    return 1;
}

int l_set_action_pressed(lua_State* L) {
    world_from_state(L).set_action_pressed(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}

int l_get_axis(lua_State* L) {
    lua_pushnumber(L, world_from_state(L).get_axis(luaL_checkstring(L, 1)));
    return 1;
}

int l_set_axis(lua_State* L) {
    world_from_state(L).set_axis(luaL_checkstring(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}

int l_spawn_box(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto sizeVoxels = static_cast<int>(luaL_checkinteger(L, 2));
    const float voxelSizeMeters = static_cast<float>(luaL_checknumber(L, 3));
    const Float3 position = check_float3(L, 4);
    const bool dynamic = lua_toboolean(L, 7) != 0;
    const double density = luaL_optnumber(L, 8, 1000.0);
    if (sizeVoxels <= 0 || sizeVoxels > 64) return luaL_error(L, "sizeVoxels must be between 1 and 64");
    if (!(voxelSizeMeters > 0.0F)) return luaL_error(L, "voxelSizeMeters must be positive");

    GameObjectDesc desc;
    desc.name = name;
    desc.voxelSizeMeters = voxelSizeMeters;
    desc.transform = make_rigid_transform(position, {});
    desc.dynamic = dynamic;
    desc.densityKilogramsPerCubicMeter = density;
    auto voxels = std::make_unique<VoxelObject>(1);
    for (int x = 0; x < sizeVoxels; ++x)
        for (int y = 0; y < sizeVoxels; ++y)
            for (int z = 0; z < sizeVoxels; ++z) voxels->set_voxel({x, y, z}, 1);
    desc.voxels = std::move(voxels);

    std::string error;
    const GameObjectId id = world_from_state(L).create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

// A non-cube variant, added specifically because world.spawn_box's cube-only shape is an easy
// mistake to make for the extremely common case of a floor/wall/platform: sizing it wide
// enough horizontally under spawn_box's single-size parameter makes it that many voxels *tall*
// too, tall enough to bury whatever gets dropped onto it (this bit two different test files
// independently while building this feature; see docs/scripting_implementation_notes.md).
int l_spawn_box3(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto sizeX = static_cast<int>(luaL_checkinteger(L, 2));
    const auto sizeY = static_cast<int>(luaL_checkinteger(L, 3));
    const auto sizeZ = static_cast<int>(luaL_checkinteger(L, 4));
    const float voxelSizeMeters = static_cast<float>(luaL_checknumber(L, 5));
    const Float3 position = check_float3(L, 6);
    const bool dynamic = lua_toboolean(L, 9) != 0;
    const double density = luaL_optnumber(L, 10, 1000.0);
    if (sizeX <= 0 || sizeX > 128 || sizeY <= 0 || sizeY > 128 || sizeZ <= 0 || sizeZ > 128)
        return luaL_error(L, "sizeX/sizeY/sizeZ must each be between 1 and 128");
    if (!(voxelSizeMeters > 0.0F)) return luaL_error(L, "voxelSizeMeters must be positive");

    GameObjectDesc desc;
    desc.name = name;
    desc.voxelSizeMeters = voxelSizeMeters;
    desc.transform = make_rigid_transform(position, {});
    desc.dynamic = dynamic;
    desc.densityKilogramsPerCubicMeter = density;
    auto voxels = std::make_unique<VoxelObject>(1);
    for (int x = 0; x < sizeX; ++x)
        for (int y = 0; y < sizeY; ++y)
            for (int z = 0; z < sizeZ; ++z) voxels->set_voxel({x, y, z}, 1);
    desc.voxels = std::move(voxels);

    std::string error;
    const GameObjectId id = world_from_state(L).create_object(std::move(desc), &error);
    if (id == kInvalidGameObjectId) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_destroy(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, world_from_state(L).destroy_object(id));
    return 1;
}

int l_pool_create_box(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto capacity = static_cast<std::size_t>(luaL_checkinteger(L, 2));
    const auto sizeX = static_cast<int>(luaL_checkinteger(L, 3));
    const auto sizeY = static_cast<int>(luaL_checkinteger(L, 4));
    const auto sizeZ = static_cast<int>(luaL_checkinteger(L, 5));
    const float voxelSizeMeters = static_cast<float>(luaL_checknumber(L, 6));
    const bool dynamic = lua_toboolean(L, 7) != 0;
    const double density = luaL_optnumber(L, 8, 1000.0);
    if (capacity == 0U || capacity > 65536U) return luaL_error(L, "capacity must be between 1 and 65536");
    if (sizeX <= 0 || sizeX > 128 || sizeY <= 0 || sizeY > 128 || sizeZ <= 0 || sizeZ > 128)
        return luaL_error(L, "sizeX/sizeY/sizeZ must each be between 1 and 128");
    if (!(voxelSizeMeters > 0.0F)) return luaL_error(L, "voxelSizeMeters must be positive");

    GameObjectPoolDesc pool;
    pool.name = name;
    pool.capacity = capacity;
    pool.prototype.name = std::string(name) + " pooled object";
    pool.prototype.voxelSizeMeters = voxelSizeMeters;
    pool.prototype.dynamic = dynamic;
    pool.prototype.densityKilogramsPerCubicMeter = density;
    pool.prototype.voxels = std::make_unique<VoxelObject>(1);
    for (int x = 0; x < sizeX; ++x)
        for (int y = 0; y < sizeY; ++y)
            for (int z = 0; z < sizeZ; ++z) pool.prototype.voxels->set_voxel({x, y, z}, 1);
    std::string error;
    const GameObjectPoolId poolId = world_from_state(L).register_pool(std::move(pool), &error);
    if (poolId == kInvalidGameObjectPoolId) {
        lua_pushnil(L); lua_pushlstring(L, error.data(), error.size()); return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(poolId));
    return 1;
}

int l_pool_acquire(lua_State* L) {
    const auto poolId = static_cast<GameObjectPoolId>(luaL_checkinteger(L, 1));
    const Float3 position = check_float3(L, 2);
    std::string error;
    const GameObjectId id = world_from_state(L).acquire_from_pool(
        poolId, make_rigid_transform(position, {}), &error);
    if (id == kInvalidGameObjectId) {
        lua_pushnil(L); lua_pushlstring(L, error.data(), error.size()); return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_pool_release(lua_State* L) {
    const auto id = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    std::string error;
    const bool success = world_from_state(L).release_to_pool(id, &error);
    lua_pushboolean(L, success);
    if (!success) { lua_pushlstring(L, error.data(), error.size()); return 2; }
    return 1;
}

int l_pool_available(lua_State* L) {
    const auto poolId = static_cast<GameObjectPoolId>(luaL_checkinteger(L, 1));
    lua_pushinteger(L, static_cast<lua_Integer>(world_from_state(L).pool_available(poolId)));
    return 1;
}

int l_spawn_asset(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const Float3 position = check_float3(L, 2);
    const bool dynamic = lua_toboolean(L, 5) != 0;
    const bool structural = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    std::string error;
    const GameObjectId id = world_from_state(L).spawn_asset(
        path, "", make_rigid_transform(position, {}), dynamic, structural, &error);
    if (id == kInvalidGameObjectId) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_schedule_once(lua_State* L) {
    const float seconds = static_cast<float>(luaL_checknumber(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    const auto id = world_from_state(L).schedule_once(seconds, [L, ref]() {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) report_callback_error(L, "timer");
        luaL_unref(L, LUA_REGISTRYINDEX, ref); // one-shot: never fires again, safe to release now
    });
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_schedule_repeating(lua_State* L) {
    const float interval = static_cast<float>(luaL_checknumber(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    const auto id = world_from_state(L).schedule_repeating(interval, [L, ref]() {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) report_callback_error(L, "timer");
    });
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_cancel_timer(lua_State* L) {
    const auto id = static_cast<GameWorld::TimerId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, world_from_state(L).cancel_timer(id));
    return 1;
}

int l_on_tick(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    impl_from_state(L)->tickRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int l_on_damage(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    impl_from_state(L)->damageRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int l_on_destroyed(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    impl_from_state(L)->destroyRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int l_on_lifecycle(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    impl_from_state(L)->lifecycleRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}


CharacterControllerConfig check_character_config(lua_State* L, int index) {
    CharacterControllerConfig config;
    if (lua_isnoneornil(L, index)) return config;
    luaL_checktype(L, index, LUA_TTABLE);
    const auto read_number = [&](const char* name, float& target) {
        lua_getfield(L, index, name);
        if (!lua_isnil(L, -1)) target = static_cast<float>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
    };
    const auto read_integer = [&](const char* name, std::uint32_t& target) {
        lua_getfield(L, index, name);
        if (!lua_isnil(L, -1)) target = static_cast<std::uint32_t>(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
    };
    read_number("standing_height", config.standingHeightMeters);
    read_number("crouched_height", config.crouchedHeightMeters);
    read_number("radius", config.radiusMeters);
    read_number("skin", config.skinMeters);
    read_number("speed", config.maximumGroundSpeedMetersPerSecond);
    read_number("ground_acceleration", config.groundAccelerationMetersPerSecondSquared);
    read_number("air_acceleration", config.airAccelerationMetersPerSecondSquared);
    read_number("braking", config.groundBrakingMetersPerSecondSquared);
    read_number("gravity", config.gravityMetersPerSecondSquared);
    read_number("jump_speed", config.jumpSpeedMetersPerSecond);
    read_number("maximum_fall_speed", config.maximumFallSpeedMetersPerSecond);
    read_number("maximum_slope_degrees", config.maximumSlopeDegrees);
    read_number("step_height", config.stepHeightMeters);
    read_number("ground_probe", config.groundProbeMeters);
    read_number("coyote_time", config.coyoteTimeSeconds);
    read_number("jump_buffer", config.jumpBufferSeconds);
    read_integer("slide_iterations", config.maximumSlideIterations);
    return config;
}

int l_character_add(lua_State* L) {
    const auto pawn = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const CharacterControllerConfig config = check_character_config(L, 2);
    std::string error;
    const bool success = world_from_state(L).gameplay().add_character(pawn, config, &error);
    lua_pushboolean(L, success);
    if (!success) { lua_pushstring(L, error.c_str()); return 2; }
    return 1;
}

int l_character_remove(lua_State* L) {
    lua_pushboolean(L, world_from_state(L).gameplay().remove_character(
        static_cast<GameObjectId>(luaL_checkinteger(L, 1))));
    return 1;
}

int l_character_set_input(lua_State* L) {
    const auto pawn = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    CharacterInput input;
    input.move = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), 0.0F};
    input.jumpPressed = lua_toboolean(L, 4) != 0;
    input.crouchHeld = lua_toboolean(L, 5) != 0;
    lua_pushboolean(L, world_from_state(L).gameplay().set_character_input(pawn, input));
    return 1;
}

int l_character_get(lua_State* L) {
    const auto pawn = static_cast<GameObjectId>(luaL_checkinteger(L, 1));
    const auto* state = world_from_state(L).gameplay().character(pawn);
    if (!state) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(state->pawn)); lua_setfield(L, -2, "pawn");
    push_float3_table(L, state->velocity); lua_setfield(L, -2, "velocity");
    lua_pushboolean(L, state->grounded); lua_setfield(L, -2, "grounded");
    lua_pushinteger(L, static_cast<lua_Integer>(state->supportObject)); lua_setfield(L, -2, "support");
    push_float3_table(L, state->groundNormal); lua_setfield(L, -2, "ground_normal");
    lua_pushstring(L, state->stance == CharacterStance::Standing ? "standing" : "crouched");
    lua_setfield(L, -2, "stance");
    lua_pushinteger(L, static_cast<lua_Integer>(state->fixedTick)); lua_setfield(L, -2, "fixed_tick");
    return 1;
}

int l_player_create(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const bool local = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    lua_pushinteger(L, static_cast<lua_Integer>(world_from_state(L).gameplay().create_player(name, local)));
    return 1;
}

int l_player_destroy(lua_State* L) {
    lua_pushboolean(L, world_from_state(L).gameplay().destroy_player(
        static_cast<GamePlayerId>(luaL_checkinteger(L, 1))));
    return 1;
}

int l_player_possess(lua_State* L) {
    const auto player = static_cast<GamePlayerId>(luaL_checkinteger(L, 1));
    const auto pawn = static_cast<GameObjectId>(luaL_checkinteger(L, 2));
    std::string error;
    const bool success = world_from_state(L).gameplay().possess(player, pawn, &error);
    lua_pushboolean(L, success);
    if (!success) { lua_pushstring(L, error.c_str()); return 2; }
    return 1;
}

int l_player_unpossess(lua_State* L) {
    lua_pushboolean(L, world_from_state(L).gameplay().unpossess(
        static_cast<GamePlayerId>(luaL_checkinteger(L, 1))));
    return 1;
}

int l_player_set_input(lua_State* L) {
    const auto player = static_cast<GamePlayerId>(luaL_checkinteger(L, 1));
    CharacterInput input;
    input.move = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), 0.0F};
    input.jumpPressed = lua_toboolean(L, 4) != 0;
    input.crouchHeld = lua_toboolean(L, 5) != 0;
    lua_pushboolean(L, world_from_state(L).gameplay().set_player_input(player, input));
    return 1;
}

int l_trigger_create_box(lua_State* L) {
    TriggerVolumeDesc desc;
    desc.name = luaL_checkstring(L, 1);
    desc.shape = TriggerShape::Box;
    desc.transform.position = check_float3(L, 2);
    desc.halfExtents = check_float3(L, 5);
    desc.oneShot = lua_toboolean(L, 8) != 0;
    if (!lua_isnoneornil(L, 9)) desc.requiredTag = luaL_checkstring(L, 9);
    std::string error;
    const GameTriggerId id = world_from_state(L).gameplay().create_trigger(std::move(desc), &error);
    if (id == kInvalidGameTriggerId) { lua_pushnil(L); lua_pushstring(L, error.c_str()); return 2; }
    lua_pushinteger(L, static_cast<lua_Integer>(id)); return 1;
}

int l_trigger_create_sphere(lua_State* L) {
    TriggerVolumeDesc desc;
    desc.name = luaL_checkstring(L, 1);
    desc.shape = TriggerShape::Sphere;
    desc.transform.position = check_float3(L, 2);
    desc.radiusMeters = static_cast<float>(luaL_checknumber(L, 5));
    desc.oneShot = lua_toboolean(L, 6) != 0;
    if (!lua_isnoneornil(L, 7)) desc.requiredTag = luaL_checkstring(L, 7);
    std::string error;
    const GameTriggerId id = world_from_state(L).gameplay().create_trigger(std::move(desc), &error);
    if (id == kInvalidGameTriggerId) { lua_pushnil(L); lua_pushstring(L, error.c_str()); return 2; }
    lua_pushinteger(L, static_cast<lua_Integer>(id)); return 1;
}

int l_trigger_set_enabled(lua_State* L) {
    lua_pushboolean(L, world_from_state(L).gameplay().set_trigger_enabled(
        static_cast<GameTriggerId>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2) != 0));
    return 1;
}

int l_trigger_fired(lua_State* L) {
    lua_pushboolean(L, world_from_state(L).gameplay().trigger_fired(
        static_cast<GameTriggerId>(luaL_checkinteger(L, 1))));
    return 1;
}

int l_on_trigger(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    impl_from_state(L)->triggerRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int l_log(lua_State* L) {
    const char* message = luaL_checkstring(L, 1);
    GameScriptHost::Impl* impl = impl_from_state(L);
    if (impl && impl->logSink) impl->logSink(false, message);
    else std::printf("[lua] %s\n", message);
    return 0;
}

int l_set_global(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const double value = luaL_checknumber(L, 2);
    GameScriptHost::Impl* impl = impl_from_state(L);
    std::string error;
    const bool feedsMaterials = impl->materials.parameter_collection().scalar_slot(key).has_value() ||
                                impl->materials.uses_global_scalar(key);
    if (feedsMaterials && !impl->materials.set_global_scalar(key, static_cast<float>(value), &error)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    // Ordinary gameplay globals remain an unbounded script dictionary. Only canonical MPC
    // names or names explicitly bound by a master consume one of the fixed GPU slots.
    impl->globals[key] = value;
    lua_pushboolean(L, 1);
    return 1;
}

int l_get_global(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    GameScriptHost::Impl* impl = impl_from_state(L);
    const auto it = impl->globals.find(key);
    if (it != impl->globals.end()) { lua_pushnumber(L, it->second); return 1; }
    if (const auto materialGlobal = impl->materials.parameter_collection().scalar(key)) {
        lua_pushnumber(L, *materialGlobal);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}


int l_set_global_vector(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    Float4 value{
        static_cast<float>(luaL_checknumber(L, 2)),
        static_cast<float>(luaL_checknumber(L, 3)),
        static_cast<float>(luaL_checknumber(L, 4)),
        static_cast<float>(luaL_optnumber(L, 5, 1.0))};
    GameScriptHost::Impl* impl = impl_from_state(L);
    std::string error;
    if (!impl->materials.set_global_vector(key, value, &error)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    impl->globalVectors[key] = value;
    lua_pushboolean(L, 1);
    return 1;
}

int l_get_global_vector(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    GameScriptHost::Impl* impl = impl_from_state(L);
    const auto it = impl->globalVectors.find(key);
    if (it != impl->globalVectors.end()) { push_float4_table(L, it->second); return 1; }
    if (const auto materialGlobal = impl->materials.parameter_collection().vector(key)) {
        push_float4_table(L, *materialGlobal);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int l_hud_set_interaction_prompt(lua_State* L) {
    impl_from_state(L)->hud.set_interaction_prompt(luaL_checkstring(L, 1));
    return 0;
}

int l_hud_set_tools(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<ui::ToolWheelEntry> tools;
    const lua_Integer count = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "id");
        const char* toolId = luaL_checkstring(L, -1);
        lua_getfield(L, -2, "label");
        const char* label = luaL_checkstring(L, -1);
        lua_getfield(L, -3, "enabled");
        const bool enabled = lua_isnil(L, -1) ? true : (lua_toboolean(L, -1) != 0);
        tools.push_back({toolId, label, enabled});
        lua_pop(L, 4); // enabled, label, id, the entry table
    }
    impl_from_state(L)->hud.set_tools(std::move(tools));
    return 0;
}

int l_hud_select_next_tool(lua_State* L) {
    lua_pushboolean(L, impl_from_state(L)->hud.select_next_tool());
    return 1;
}

int l_hud_selected_tool(lua_State* L) {
    const auto tool = impl_from_state(L)->hud.selected_tool();
    if (!tool) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushstring(L, tool->id.c_str()); lua_setfield(L, -2, "id");
    lua_pushstring(L, tool->label.c_str()); lua_setfield(L, -2, "label");
    lua_pushboolean(L, tool->enabled); lua_setfield(L, -2, "enabled");
    return 1;
}

// --- Master material / material instance bindings -------------------------------------

double read_number_field(lua_State* L, int tableIndex, const char* field, double defaultValue) {
    lua_getfield(L, tableIndex, field);
    const double value = lua_isnil(L, -1) ? defaultValue : luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return value;
}
bool read_bool_field(lua_State* L, int tableIndex, const char* field, bool defaultValue) {
    lua_getfield(L, tableIndex, field);
    const bool value = lua_isnil(L, -1) ? defaultValue : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return value;
}
std::string read_string_field(lua_State* L, int tableIndex, const char* field, const char* defaultValue) {
    lua_getfield(L, tableIndex, field);
    std::string value = lua_isnil(L, -1) ? defaultValue : luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return value;
}
// Reads a {x,y,z,w}-style array sub-table at `field`; missing components default to 0 except
// a missing 4th (alpha/w), which defaults to 1 (matching VoxelMaterialDefinition::baseColor's
// own default), since most callers writing {r,g,b} for an opaque color would otherwise get a
// silently-invisible (alpha=0) result.
Float4 read_float4_field(lua_State* L, int tableIndex, const char* field, Float4 defaultValue) {
    lua_getfield(L, tableIndex, field);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return defaultValue; }
    luaL_checktype(L, -1, LUA_TTABLE);
    Float4 result{0.0F, 0.0F, 0.0F, 1.0F};
    lua_rawgeti(L, -1, 1); result.x = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
    lua_rawgeti(L, -1, 2); result.y = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
    lua_rawgeti(L, -1, 3); result.z = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
    lua_rawgeti(L, -1, 4); result.w = static_cast<float>(luaL_optnumber(L, -1, 1.0)); lua_pop(L, 1);
    lua_pop(L, 1);
    return result;
}
MaterialShadingModel parse_shading_model(std::string_view name) {
    if (name == "unlit") return MaterialShadingModel::Unlit;
    if (name == "emissive") return MaterialShadingModel::Emissive;
    if (name == "subsurface") return MaterialShadingModel::Subsurface;
    if (name == "two_sided_foliage") return MaterialShadingModel::TwoSidedFoliage;
    if (name == "clear_coat") return MaterialShadingModel::ClearCoat;
    return MaterialShadingModel::StandardPBR;
}
MaterialGlobalCombine parse_global_combine(std::string_view name) {
    if (name == "add") return MaterialGlobalCombine::Add;
    if (name == "multiply") return MaterialGlobalCombine::Multiply;
    return MaterialGlobalCombine::Replace;
}
MaterialLayerBlendMode parse_layer_blend(std::string_view name) {
    if (name == "multiply") return MaterialLayerBlendMode::Multiply;
    if (name == "additive") return MaterialLayerBlendMode::Additive;
    return MaterialLayerBlendMode::Lerp;
}
MaterialBlendMode parse_blend_mode(std::string_view name) {
    if (name == "masked") return MaterialBlendMode::Masked;
    if (name == "translucent") return MaterialBlendMode::Translucent;
    return MaterialBlendMode::Opaque;
}

// Reads scalars/vectors/switches array sub-tables ({ {name=,default=,min=,max=}, ... }) at
// `field` into a MaterialParameterSchema. Used only for master material creation, where each
// entry needs several fields, hence array-of-tables rather than a plain name-keyed table.
void read_schema_field(lua_State* L, int tableIndex, MaterialParameterSchema& schema) {
    lua_getfield(L, tableIndex, "scalars");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            const int entry = lua_gettop(L);
            schema.scalars.push_back({
                read_string_field(L, entry, "name", ""),
                static_cast<float>(read_number_field(L, entry, "default", 0.0)),
                static_cast<float>(read_number_field(L, entry, "min", 0.0)),
                static_cast<float>(read_number_field(L, entry, "max", 1.0))});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "vectors");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            const int entry = lua_gettop(L);
            schema.vectors.push_back({
                read_string_field(L, entry, "name", ""),
                read_float4_field(L, entry, "default", {1.0F, 1.0F, 1.0F, 1.0F})});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "switches");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            const int entry = lua_gettop(L);
            schema.switches.push_back({read_string_field(L, entry, "name", ""), read_bool_field(L, entry, "default", false)});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}


void read_global_bindings(lua_State* L, int tableIndex, MasterMaterial& master) {
    lua_getfield(L, tableIndex, "global_scalars");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i); luaL_checktype(L, -1, LUA_TTABLE);
            const int entry = lua_gettop(L);
            master.globalScalars.push_back({
                read_string_field(L, entry, "parameter", ""),
                read_string_field(L, entry, "global", ""),
                parse_global_combine(read_string_field(L, entry, "combine", "replace"))});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "global_vectors");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i); luaL_checktype(L, -1, LUA_TTABLE);
            const int entry = lua_gettop(L);
            master.globalVectors.push_back({
                read_string_field(L, entry, "parameter", ""),
                read_string_field(L, entry, "global", ""),
                parse_global_combine(read_string_field(L, entry, "combine", "multiply"))});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int l_create_master_material(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    MasterMaterial master;
    master.name = name;
    master.shadingModel = parse_shading_model(read_string_field(L, 2, "shading_model", "standard_pbr"));
    master.blendMode = parse_blend_mode(read_string_field(L, 2, "blend_mode", "opaque"));
    read_schema_field(L, 2, master.parameters);
    read_global_bindings(L, 2, master);

    std::string error;
    const MasterMaterialId id = impl_from_state(L)->materials.add_master(std::move(master), &error);
    if (id == kInvalidMasterMaterialId) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_create_material_instance(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    MaterialInstance instance;
    instance.name = read_string_field(L, 1, "name", "");
    instance.materialId = static_cast<MaterialId>(read_number_field(L, 1, "material_id", 0.0));
    instance.master = static_cast<MasterMaterialId>(read_number_field(L, 1, "master", 0.0));
    const double parent = read_number_field(L, 1, "parent", -1.0);
    if (parent >= 0.0) instance.parent = static_cast<MaterialInstanceId>(parent);
    instance.densityKilogramsPerCubicMeter = static_cast<float>(read_number_field(L, 1, "density", 1000.0));
    instance.structuralStrength = static_cast<float>(read_number_field(L, 1, "structural_strength", 1.0));
    instance.fractureResistance = static_cast<float>(read_number_field(L, 1, "fracture_resistance", 1.0));
    instance.flammability = static_cast<float>(read_number_field(L, 1, "flammability", 0.0));
    instance.thermalConductivity = static_cast<float>(read_number_field(L, 1, "thermal_conductivity", 0.0));
    instance.structural = read_bool_field(L, 1, "structural", true);

    lua_getfield(L, 1, "scalars");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char* key = luaL_checkstring(L, -2);
            instance.scalarOverrides[key] = static_cast<float>(luaL_checknumber(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "vectors");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char* key = luaL_checkstring(L, -2);
            luaL_checktype(L, -1, LUA_TTABLE);
            Float4 value{0.0F, 0.0F, 0.0F, 1.0F};
            lua_rawgeti(L, -1, 1); value.x = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
            lua_rawgeti(L, -1, 2); value.y = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
            lua_rawgeti(L, -1, 3); value.z = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
            lua_rawgeti(L, -1, 4); value.w = static_cast<float>(luaL_optnumber(L, -1, 1.0)); lua_pop(L, 1);
            instance.vectorOverrides[key] = value;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "layers");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer count = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i); luaL_checktype(L, -1, LUA_TTABLE);
            const int entry = lua_gettop(L);
            VoxelMaterialLayer layer;
            layer.sourceMaterial = static_cast<MaterialId>(read_number_field(L, entry, "material_id", 0.0));
            layer.weight = static_cast<float>(read_number_field(L, entry, "weight", 1.0));
            layer.blendMode = parse_layer_blend(read_string_field(L, entry, "blend", "lerp"));
            layer.enabled = read_bool_field(L, entry, "enabled", true);
            instance.layers.push_back(layer);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    std::string error;
    const MaterialInstanceId id = impl_from_state(L)->materials.add_instance(std::move(instance), &error);
    if (id == kInvalidMaterialInstanceId) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_set_material_parameter(lua_State* L) {
    const auto materialId = static_cast<MaterialId>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    MaterialLibrary& materials = impl_from_state(L)->materials;
    std::string error;
    bool ok;
    if (lua_istable(L, 3)) {
        Float4 value{0.0F, 0.0F, 0.0F, 1.0F};
        lua_rawgeti(L, 3, 1); value.x = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
        lua_rawgeti(L, 3, 2); value.y = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
        lua_rawgeti(L, 3, 3); value.z = static_cast<float>(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
        lua_rawgeti(L, 3, 4); value.w = static_cast<float>(luaL_optnumber(L, -1, 1.0)); lua_pop(L, 1);
        ok = materials.set_runtime_vector(materialId, name, value, &error);
    } else {
        ok = materials.set_runtime_scalar(materialId, name, static_cast<float>(luaL_checknumber(L, 3)), &error);
    }
    if (!ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_get_material_parameter(lua_State* L) {
    const auto materialId = static_cast<MaterialId>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const MaterialLibrary& materials = impl_from_state(L)->materials;
    if (const auto scalar = materials.runtime_scalar(materialId, name)) { lua_pushnumber(L, *scalar); return 1; }
    if (const auto vector = materials.runtime_vector(materialId, name)) { push_float3_table(L, {vector->x, vector->y, vector->z}); return 1; }
    // Not runtime-overridden: fall back to the instance-resolved value, checking the flat
    // VoxelMaterialDefinition for the small set of reserved names it actually stores.
    if (const VoxelMaterialDefinition* resolved = materials.resolved(materialId)) {
        const std::string_view field = name;
        if (field == kRoughnessParam) { lua_pushnumber(L, resolved->roughness); return 1; }
        if (field == kMetallicParam) { lua_pushnumber(L, resolved->metallic); return 1; }
        if (field == kSpecularParam) { lua_pushnumber(L, resolved->specular); return 1; }
        if (field == kSubsurfaceScatterDistanceParam) { lua_pushnumber(L, resolved->subsurfaceScatterDistanceMeters); return 1; }
        if (field == kClearCoatParam) { lua_pushnumber(L, resolved->clearCoat); return 1; }
        if (field == kClearCoatRoughnessParam) { lua_pushnumber(L, resolved->clearCoatRoughness); return 1; }
        if (field == kFoliageTransmittanceParam) { lua_pushnumber(L, resolved->foliageTransmittance); return 1; }
        if (field == kFoliageWrapParam) { lua_pushnumber(L, resolved->foliageWrap); return 1; }
        if (field == kBaseColorParam) { push_float4_table(L, resolved->baseColor); return 1; }
        if (field == kEmissiveParam) { push_float3_table(L, resolved->emissive); return 1; }
        if (field == kSubsurfaceColorParam) { push_float3_table(L, resolved->subsurfaceColor); return 1; }
        if (field == kFoliageColorParam) { push_float3_table(L, resolved->foliageColor); return 1; }
    }
    lua_pushnil(L);
    return 1;
}

int l_clear_material_parameters(lua_State* L) {
    const auto materialId = static_cast<MaterialId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, impl_from_state(L)->materials.clear_runtime_overrides(materialId));
    return 1;
}

int l_set_material_layer_weight(lua_State* L) {
    const auto materialId = static_cast<MaterialId>(luaL_checkinteger(L, 1));
    const lua_Integer oneBased = luaL_checkinteger(L, 2);
    if (oneBased <= 0) { lua_pushboolean(L, 0); lua_pushstring(L, "layer index is one-based"); return 2; }
    std::string error;
    const bool ok = impl_from_state(L)->materials.set_runtime_layer_weight(
        materialId, static_cast<std::size_t>(oneBased - 1), static_cast<float>(luaL_checknumber(L, 3)), &error);
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, error.c_str()); return 2; }
    return 1;
}

int l_get_material_layer_weight(lua_State* L) {
    const auto materialId = static_cast<MaterialId>(luaL_checkinteger(L, 1));
    const lua_Integer oneBased = luaL_checkinteger(L, 2);
    if (oneBased <= 0) { lua_pushnil(L); return 1; }
    const auto value = impl_from_state(L)->materials.runtime_layer_weight(
        materialId, static_cast<std::size_t>(oneBased - 1));
    if (!value) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, *value);
    return 1;
}

// --- Render environment bindings (Material-Parameter-Collection / Godot-Environment-resource
// equivalent) -------------------------------------------------------------------------------

int l_set_environment_scalar(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const float value = static_cast<float>(luaL_checknumber(L, 2));
    RenderEnvironment& environment = impl_from_state(L)->environment;
    const RenderEnvironment previous = environment;
    bool known = true;
    if (name == "SunIntensity") environment.sunIntensity = value;
    else if (name == "SubsurfaceMaxDistanceMeters") environment.subsurfaceMaxDistanceMeters = value;
    else if (name == "Exposure") environment.exposure = value;
    else if (name == "BloomThreshold") environment.bloomThreshold = value;
    else if (name == "BloomIntensity") environment.bloomIntensity = value;
    else if (name == "BloomRadius") environment.bloomRadius = value;
    else if (name == "GlobalIlluminationIntensity") environment.globalIlluminationIntensity = value;
    else if (name == "GlobalIlluminationMaxDistanceMeters") environment.globalIlluminationMaxDistanceMeters = value;
    else if (name == "GlobalIlluminationSamples") environment.globalIlluminationSamples = static_cast<std::uint32_t>(std::max(0.0F, std::round(value)));
    else if (name == "ShadowStrength") environment.shadowStrength = value;
    else if (name == "ShadowSoftnessRadians") environment.shadowSoftnessRadians = value;
    else if (name == "ShadowSamples") environment.shadowSamples = static_cast<std::uint32_t>(std::max(0.0F, std::round(value)));
    else if (name == "ShadowMaxDistanceMeters") environment.shadowMaxDistanceMeters = value;
    else if (name == "ContactShadowDistanceMeters") environment.contactShadowDistanceMeters = value;
    else if (name == "ShadowBiasMeters") environment.shadowBiasMeters = value;
    else known = false;
    if (!known) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ("unknown environment scalar parameter \"" + name + "\"").c_str());
        return 2;
    }
    std::string error;
    if (!environment.validate(&error)) {
        environment = previous; // reject: leave the environment exactly as it was, not half-applied
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_set_environment_vector(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    const Float3 value = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                           static_cast<float>(luaL_checknumber(L, 4))};
    RenderEnvironment& environment = impl_from_state(L)->environment;
    const RenderEnvironment previous = environment;
    bool known = true;
    if (name == "SunDirection") environment.sunDirection = value;
    else if (name == "SunColor") environment.sunColor = value;
    else if (name == "SkyColor") environment.skyColor = value;
    else if (name == "GroundColor") environment.groundColor = value;
    else if (name == "GlobalTint") environment.globalTint = value;
    else known = false;
    if (!known) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ("unknown environment vector parameter \"" + name + "\"").c_str());
        return 2;
    }
    std::string error;
    if (!environment.validate(&error)) {
        environment = previous;
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_set_environment_tonemap_operator(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    RenderEnvironment& environment = impl_from_state(L)->environment;
    if (name == "aces") environment.tonemapOperator = TonemapOperator::ACES;
    else if (name == "reinhard") environment.tonemapOperator = TonemapOperator::Reinhard;
    else if (name == "clamp") environment.tonemapOperator = TonemapOperator::Clamp;
    else {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "tonemap operator must be \"aces\", \"reinhard\", or \"clamp\"");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_set_environment_gi_mode(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    RenderEnvironment& environment = impl_from_state(L)->environment;
    if (name == "off") environment.globalIlluminationMode = GlobalIlluminationMode::Off;
    else if (name == "ambient") environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    else if (name == "voxel_one_bounce") environment.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
    else {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "GI mode must be \"off\", \"ambient\", or \"voxel_one_bounce\"");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_set_environment_shadow_mode(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    RenderEnvironment& environment = impl_from_state(L)->environment;
    if (name == "off") environment.shadowMode = ShadowMode::Off;
    else if (name == "hard") environment.shadowMode = ShadowMode::Hard;
    else if (name == "soft") environment.shadowMode = ShadowMode::Soft;
    else if (name == "contact") environment.shadowMode = ShadowMode::Contact;
    else if (name == "hybrid") environment.shadowMode = ShadowMode::Hybrid;
    else {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "shadow mode must be \"off\", \"hard\", \"soft\", \"contact\", or \"hybrid\"");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_get_environment(lua_State* L) {
    const RenderEnvironment& environment = impl_from_state(L)->environment;
    lua_newtable(L);
    push_float3_table(L, environment.sunDirection); lua_setfield(L, -2, "sun_direction");
    lua_pushnumber(L, environment.sunIntensity); lua_setfield(L, -2, "sun_intensity");
    push_float3_table(L, environment.sunColor); lua_setfield(L, -2, "sun_color");
    push_float3_table(L, environment.skyColor); lua_setfield(L, -2, "sky_color");
    push_float3_table(L, environment.groundColor); lua_setfield(L, -2, "ground_color");
    push_float3_table(L, environment.globalTint); lua_setfield(L, -2, "global_tint");
    lua_pushnumber(L, environment.exposure); lua_setfield(L, -2, "exposure");
    lua_pushnumber(L, environment.bloomThreshold); lua_setfield(L, -2, "bloom_threshold");
    lua_pushnumber(L, environment.bloomIntensity); lua_setfield(L, -2, "bloom_intensity");
    lua_pushnumber(L, environment.bloomRadius); lua_setfield(L, -2, "bloom_radius");
    lua_pushnumber(L, environment.subsurfaceMaxDistanceMeters); lua_setfield(L, -2, "subsurface_max_distance_meters");
    const char* giName = environment.globalIlluminationMode == GlobalIlluminationMode::Off ? "off"
                         : environment.globalIlluminationMode == GlobalIlluminationMode::AmbientHemisphere ? "ambient"
                         : "voxel_one_bounce";
    lua_pushstring(L, giName); lua_setfield(L, -2, "global_illumination_mode");
    lua_pushnumber(L, environment.globalIlluminationIntensity); lua_setfield(L, -2, "global_illumination_intensity");
    lua_pushnumber(L, environment.globalIlluminationMaxDistanceMeters); lua_setfield(L, -2, "global_illumination_max_distance_meters");
    lua_pushinteger(L, environment.globalIlluminationSamples); lua_setfield(L, -2, "global_illumination_samples");
    const char* shadowName = environment.shadowMode == ShadowMode::Off ? "off"
                             : environment.shadowMode == ShadowMode::Hard ? "hard"
                             : environment.shadowMode == ShadowMode::Soft ? "soft"
                             : environment.shadowMode == ShadowMode::Contact ? "contact" : "hybrid";
    lua_pushstring(L, shadowName); lua_setfield(L, -2, "shadow_mode");
    lua_pushnumber(L, environment.shadowStrength); lua_setfield(L, -2, "shadow_strength");
    lua_pushnumber(L, environment.shadowSoftnessRadians); lua_setfield(L, -2, "shadow_softness_radians");
    lua_pushinteger(L, environment.shadowSamples); lua_setfield(L, -2, "shadow_samples");
    lua_pushnumber(L, environment.shadowMaxDistanceMeters); lua_setfield(L, -2, "shadow_max_distance_meters");
    lua_pushnumber(L, environment.contactShadowDistanceMeters); lua_setfield(L, -2, "contact_shadow_distance_meters");
    lua_pushnumber(L, environment.shadowBiasMeters); lua_setfield(L, -2, "shadow_bias_meters");
    const char* tonemapName = environment.tonemapOperator == TonemapOperator::ACES ? "aces"
                              : environment.tonemapOperator == TonemapOperator::Reinhard ? "reinhard" : "clamp";
    lua_pushstring(L, tonemapName); lua_setfield(L, -2, "tonemap_operator");
    return 1;
}


int l_camera_force_live(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const auto rig = static_cast<camera::CameraRigId>(luaL_checkinteger(L, 2));
    const bool cut = lua_toboolean(L, 3) != 0;
    auto* director = world_from_state(L).cameras().director(viewport);
    lua_pushboolean(L, director && director->force_live(rig, cut));
    return 1;
}

int l_camera_clear_forced_live(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const bool cut = lua_toboolean(L, 2) != 0;
    auto* director = world_from_state(L).cameras().director(viewport);
    if (!director) { lua_pushboolean(L, 0); return 1; }
    director->clear_forced_live(cut);
    lua_pushboolean(L, 1);
    return 1;
}

int l_camera_set_state(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const char* state = luaL_checkstring(L, 2);
    auto* director = world_from_state(L).cameras().director(viewport);
    lua_pushboolean(L, director && director->set_state(state));
    return 1;
}

int l_camera_start_shake(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    camera::CameraShake shake;
    shake.id = static_cast<camera::CameraShakeId>(luaL_checkinteger(L, 2));
    const float position = static_cast<float>(luaL_checknumber(L, 3));
    const float rotationDegrees = static_cast<float>(luaL_checknumber(L, 4));
    shake.durationSeconds = static_cast<float>(luaL_checknumber(L, 5));
    shake.frequencyHertz = static_cast<float>(luaL_optnumber(L, 6, 12.0));
    shake.positionAmplitudeMeters = {position, position, position};
    const float rotation = rotationDegrees * camera::kDegreesToRadians;
    shake.rotationAmplitudeRadians = {rotation, rotation, rotation * 0.6F};
    shake.blendInSeconds = std::min(0.05F, shake.durationSeconds * 0.2F);
    shake.blendOutSeconds = std::min(0.12F, shake.durationSeconds * 0.35F);
    auto* director = world_from_state(L).cameras().director(viewport);
    std::string error;
    if (!director || !director->start_shake(shake, &error)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, director ? error.c_str() : "unknown camera viewport");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_camera_stop_shake(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const auto shake = static_cast<camera::CameraShakeId>(luaL_checkinteger(L, 2));
    auto* director = world_from_state(L).cameras().director(viewport);
    lua_pushboolean(L, director && director->stop_shake(shake));
    return 1;
}

int l_camera_load_sequence(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const std::filesystem::path path = luaL_checkstring(L, 2);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        lua_pushboolean(L, 0); lua_pushstring(L, "could not open camera sequence"); return 2;
    }
    std::ostringstream buffer; buffer << input.rdbuf();
    std::string error;
    const auto sequence = camera::CameraSequence::parse(buffer.str(), &error);
    if (!sequence || !world_from_state(L).cameras().set_sequence(viewport, *sequence, &error)) {
        lua_pushboolean(L, 0); lua_pushstring(L, error.c_str()); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_camera_play_sequence(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const bool loop = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, world_from_state(L).cameras().play_sequence(viewport, loop));
    return 1;
}
int l_camera_pause_sequence(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, world_from_state(L).cameras().pause_sequence(viewport)); return 1;
}
int l_camera_stop_sequence(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, world_from_state(L).cameras().stop_sequence(viewport)); return 1;
}
int l_camera_seek_sequence(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const float time = static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, world_from_state(L).cameras().seek_sequence(viewport, time)); return 1;
}

int l_camera_set_accessibility(lua_State* L) {
    const std::string preset = luaL_checkstring(L, 1);
    camera::CameraAccessibilitySettings settings;
    if (preset == "standard") settings.preset = camera::CameraAccessibilityPreset::Standard;
    else if (preset == "reduced_motion") settings.preset = camera::CameraAccessibilityPreset::ReducedMotion;
    else if (preset == "photosensitive") settings.preset = camera::CameraAccessibilityPreset::Photosensitive;
    else { lua_pushboolean(L, 0); lua_pushstring(L, "unknown camera accessibility preset"); return 2; }
    settings.horizonLock = lua_toboolean(L, 2) != 0;
    settings.motionScale = static_cast<float>(luaL_optnumber(L, 3, 1.0));
    settings.shakeScale = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    settings.bloomScale = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    settings.maximumDepthOfFieldWeight = static_cast<float>(luaL_optnumber(L, 6, 1.0));
    std::string error;
    if (!world_from_state(L).cameras().set_accessibility(settings, &error)) {
        lua_pushboolean(L, 0); lua_pushstring(L, error.c_str()); return 2;
    }
    lua_pushboolean(L, 1); return 1;
}

int l_camera_get_accessibility(lua_State* L) {
    const auto& settings = world_from_state(L).cameras().accessibility();
    const char* preset = settings.preset == camera::CameraAccessibilityPreset::Standard ? "standard"
        : settings.preset == camera::CameraAccessibilityPreset::ReducedMotion ? "reduced_motion"
        : "photosensitive";
    lua_newtable(L);
    lua_pushstring(L, preset); lua_setfield(L, -2, "preset");
    lua_pushboolean(L, settings.horizonLock); lua_setfield(L, -2, "horizon_lock");
    lua_pushnumber(L, settings.motionScale); lua_setfield(L, -2, "motion_scale");
    lua_pushnumber(L, settings.shakeScale); lua_setfield(L, -2, "shake_scale");
    lua_pushnumber(L, settings.bloomScale); lua_setfield(L, -2, "bloom_scale");
    lua_pushnumber(L, settings.maximumDepthOfFieldWeight); lua_setfield(L, -2, "maximum_dof_weight");
    return 1;
}

int l_camera_get(lua_State* L) {
    const auto viewport = static_cast<camera::CameraViewportId>(luaL_checkinteger(L, 1));
    const auto* frame = world_from_state(L).cameras().frame(viewport);
    if (!frame) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    push_float3_table(L, frame->pose.position); lua_setfield(L, -2, "position");
    push_float3_table(L, frame->pose.target); lua_setfield(L, -2, "target");
    lua_pushnumber(L, frame->pose.lens.effective_vertical_field_of_view_radians() * camera::kRadiansToDegrees);
    lua_setfield(L, -2, "field_of_view_degrees");
    lua_pushnumber(L, world_from_state(L).cameras().sequence_time(viewport)); lua_setfield(L, -2, "sequence_time");
    lua_pushboolean(L, world_from_state(L).cameras().sequence_playing(viewport)); lua_setfield(L, -2, "sequence_playing");
    if (frame->activeShotId) { lua_pushinteger(L, static_cast<lua_Integer>(*frame->activeShotId)); lua_setfield(L, -2, "shot_id"); }
    lua_pushstring(L, frame->activeMarker.c_str()); lua_setfield(L, -2, "marker");
    if (const auto* director = world_from_state(L).cameras().director(viewport)) {
        const auto& telemetry = director->telemetry();
        lua_pushinteger(L, static_cast<lua_Integer>(telemetry.collisionCorrections)); lua_setfield(L, -2, "collision_corrections");
        lua_pushboolean(L, telemetry.shoulderSwapActive); lua_setfield(L, -2, "shoulder_swap_active");
        lua_pushboolean(L, telemetry.occluderFadeActive); lua_setfield(L, -2, "occluder_fade_active");
        lua_pushboolean(L, telemetry.volumeConstraintActive); lua_setfield(L, -2, "volume_constraint_active");
    }
    lua_newtable(L);
    for (std::size_t index = 0; index < frame->triggeredEvents.size(); ++index) {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(frame->triggeredEvents[index].id)); lua_setfield(L, -2, "id");
        lua_pushstring(L, frame->triggeredEvents[index].name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, frame->triggeredEvents[index].payload.c_str()); lua_setfield(L, -2, "payload");
        lua_rawseti(L, -2, static_cast<lua_Integer>(index + 1U));
    }
    lua_setfield(L, -2, "events");
    return 1;
}

int l_camera_save_runtime_state(lua_State* L) {
    const std::filesystem::path path = luaL_checkstring(L, 1);
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) { lua_pushboolean(L, 0); lua_pushstring(L, "could not open camera state output"); return 2; }
        output << world_from_state(L).cameras().serialize_state();
        if (!output.good()) { lua_pushboolean(L, 0); lua_pushstring(L, "could not write camera state"); return 2; }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) { std::filesystem::remove(temporary); lua_pushboolean(L, 0); lua_pushstring(L, ec.message().c_str()); return 2; }
    lua_pushboolean(L, 1); return 1;
}

int l_camera_load_runtime_state(lua_State* L) {
    const std::filesystem::path path = luaL_checkstring(L, 1);
    std::ifstream input(path, std::ios::binary);
    if (!input) { lua_pushboolean(L, 0); lua_pushstring(L, "could not open camera state"); return 2; }
    std::ostringstream buffer; buffer << input.rdbuf(); std::string error;
    if (!world_from_state(L).cameras().restore_state(buffer.str(), &error)) {
        lua_pushboolean(L, 0); lua_pushstring(L, error.c_str()); return 2;
    }
    lua_pushboolean(L, 1); return 1;
}

constexpr luaL_Reg kWorldFunctions[] = {
    {"find_by_name", l_find_by_name},
    {"find_by_tag", l_find_by_tag},
    {"find_by_group", l_find_by_group},
    {"find_by_layer", l_find_by_layer},
    {"is_enabled", l_is_enabled},
    {"set_enabled", l_set_enabled},
    {"find_by_component", l_find_by_component},
    {"get_components", l_get_components},
    {"get_component", l_get_component},
    {"add_component", l_add_component},
    {"set_component_property", l_set_component_property},
    {"remove_component", l_remove_component},
    {"set_component_enabled", l_set_component_enabled},
    {"get_position", l_get_position},
    {"set_position", l_set_position},
    {"get_rotation", l_get_rotation},
    {"set_rotation", l_set_rotation},
    {"get_velocity", l_get_velocity},
    {"set_velocity", l_set_velocity},
    {"apply_impulse", l_apply_impulse},
    {"apply_force", l_apply_force},
    {"damage_sphere", l_damage_sphere},
    {"raycast", l_raycast},
    {"sphere_overlap", l_sphere_overlap},
    {"character_add", l_character_add},
    {"character_remove", l_character_remove},
    {"character_set_input", l_character_set_input},
    {"character_get", l_character_get},
    {"player_create", l_player_create},
    {"player_destroy", l_player_destroy},
    {"player_possess", l_player_possess},
    {"player_unpossess", l_player_unpossess},
    {"player_set_input", l_player_set_input},
    {"trigger_create_box", l_trigger_create_box},
    {"trigger_create_sphere", l_trigger_create_sphere},
    {"trigger_set_enabled", l_trigger_set_enabled},
    {"trigger_fired", l_trigger_fired},
    {"on_trigger", l_on_trigger},
    {"spawn_box", l_spawn_box},
    {"spawn_box3", l_spawn_box3},
    {"spawn_asset", l_spawn_asset},
    {"destroy", l_destroy},
    {"pool_create_box", l_pool_create_box},
    {"pool_acquire", l_pool_acquire},
    {"pool_release", l_pool_release},
    {"pool_available", l_pool_available},
    {"schedule_once", l_schedule_once},
    {"schedule_repeating", l_schedule_repeating},
    {"cancel_timer", l_cancel_timer},
    {"on_tick", l_on_tick},
    {"on_damage", l_on_damage},
    {"on_destroyed", l_on_destroyed},
    {"on_lifecycle", l_on_lifecycle},
    {"log", l_log},
    {"set_global", l_set_global},
    {"get_global", l_get_global},
    {"set_global_vector", l_set_global_vector},
    {"get_global_vector", l_get_global_vector},
    {"hud_set_interaction_prompt", l_hud_set_interaction_prompt},
    {"hud_set_tools", l_hud_set_tools},
    {"hud_select_next_tool", l_hud_select_next_tool},
    {"hud_selected_tool", l_hud_selected_tool},
    {"create_master_material", l_create_master_material},
    {"create_material_instance", l_create_material_instance},
    {"set_material_parameter", l_set_material_parameter},
    {"get_material_parameter", l_get_material_parameter},
    {"clear_material_parameters", l_clear_material_parameters},
    {"set_material_layer_weight", l_set_material_layer_weight},
    {"get_material_layer_weight", l_get_material_layer_weight},
    {"set_environment_scalar", l_set_environment_scalar},
    {"set_environment_vector", l_set_environment_vector},
    {"set_environment_tonemap_operator", l_set_environment_tonemap_operator},
    {"set_environment_gi_mode", l_set_environment_gi_mode},
    {"set_environment_shadow_mode", l_set_environment_shadow_mode},
    {"get_environment", l_get_environment},
    {"camera_force_live", l_camera_force_live},
    {"camera_clear_forced_live", l_camera_clear_forced_live},
    {"camera_set_state", l_camera_set_state},
    {"camera_start_shake", l_camera_start_shake},
    {"camera_stop_shake", l_camera_stop_shake},
    {"camera_load_sequence", l_camera_load_sequence},
    {"camera_play_sequence", l_camera_play_sequence},
    {"camera_pause_sequence", l_camera_pause_sequence},
    {"camera_stop_sequence", l_camera_stop_sequence},
    {"camera_seek_sequence", l_camera_seek_sequence},
    {"camera_set_accessibility", l_camera_set_accessibility},
    {"camera_get_accessibility", l_camera_get_accessibility},
    {"camera_get", l_camera_get},
    {"camera_save_runtime_state", l_camera_save_runtime_state},
    {"camera_load_runtime_state", l_camera_load_runtime_state},
    {"is_action_pressed", l_is_action_pressed},
    {"set_action_pressed", l_set_action_pressed},
    {"get_axis", l_get_axis},
    {"set_axis", l_set_axis},
    {nullptr, nullptr},
};

} // namespace

GameScriptHost::GameScriptHost(GameWorld& world) : impl_(std::make_unique<Impl>()) {
    impl_->world = &world;
    impl_->L = luaL_newstate();
    luaL_openlibs(impl_->L);

    lua_pushlightuserdata(impl_->L, impl_.get());
    lua_setfield(impl_->L, LUA_REGISTRYINDEX, "__dve_host_impl");

    lua_newtable(impl_->L);
    luaL_setfuncs(impl_->L, kWorldFunctions, 0);
    lua_setglobal(impl_->L, "world");

    Impl* impl = impl_.get();
    world.on_tick([impl](float dt) {
        for (int ref : impl->tickRefs) {
            lua_rawgeti(impl->L, LUA_REGISTRYINDEX, ref);
            lua_pushnumber(impl->L, dt);
            if (lua_pcall(impl->L, 1, 0, 0) != LUA_OK) report_callback_error(impl->L, "on_tick");
        }
    });
    world.on_damage([impl](const GameDamageEvent& event) {
        for (int ref : impl->damageRefs) {
            lua_rawgeti(impl->L, LUA_REGISTRYINDEX, ref);
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.objectId));
            push_float3_table(impl->L, event.worldCenter);
            lua_pushnumber(impl->L, event.radius);
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.removedVoxelCount));
            lua_pushboolean(impl->L, event.destroyed);
            lua_newtable(impl->L);
            for (std::size_t i = 0; i < event.newFragmentIds.size(); ++i) {
                lua_pushinteger(impl->L, static_cast<lua_Integer>(event.newFragmentIds[i]));
                lua_rawseti(impl->L, -2, static_cast<int>(i + 1));
            }
            if (lua_pcall(impl->L, 6, 0, 0) != LUA_OK) report_callback_error(impl->L, "on_damage");
        }
    });
    world.on_destroyed([impl](GameObjectId id) {
        for (int ref : impl->destroyRefs) {
            lua_rawgeti(impl->L, LUA_REGISTRYINDEX, ref);
            lua_pushinteger(impl->L, static_cast<lua_Integer>(id));
            if (lua_pcall(impl->L, 1, 0, 0) != LUA_OK) report_callback_error(impl->L, "on_destroyed");
        }
    });
    world.on_lifecycle([impl](const GameLifecycleEvent& event) {
        for (int ref : impl->lifecycleRefs) {
            lua_rawgeti(impl->L, LUA_REGISTRYINDEX, ref);
            lua_pushstring(impl->L, lifecycle_kind_name(event.kind));
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.objectId));
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.otherObjectId));
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.componentId));
            lua_pushlstring(impl->L, event.componentType.data(), event.componentType.size());
            lua_pushlstring(impl->L, event.poolName.data(), event.poolName.size());
            if (lua_pcall(impl->L, 6, 0, 0) != LUA_OK) report_callback_error(impl->L, "on_lifecycle");
        }
    });
    world.gameplay().on_trigger([impl](const TriggerEvent& event) {
        const char* kind = event.kind == TriggerEventKind::Enter ? "enter"
            : event.kind == TriggerEventKind::Stay ? "stay" : "exit";
        for (int ref : impl->triggerRefs) {
            lua_rawgeti(impl->L, LUA_REGISTRYINDEX, ref);
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.trigger));
            lua_pushstring(impl->L, kind);
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.object));
            lua_pushinteger(impl->L, static_cast<lua_Integer>(event.fixedTick));
            if (lua_pcall(impl->L, 4, 0, 0) != LUA_OK) report_callback_error(impl->L, "on_trigger");
        }
    });
}

GameScriptHost::~GameScriptHost() {
    if (impl_ && impl_->L) lua_close(impl_->L);
}

bool GameScriptHost::run_string(const std::string& code, const std::string& chunkName, std::string* error) {
    lua_State* L = impl_->L;
    if (luaL_loadbuffer(L, code.data(), code.size(), chunkName.c_str()) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        if (error) *error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void GameScriptHost::set_log_sink(LogSink sink) { impl_->logSink = std::move(sink); }

bool GameScriptHost::run_file(const std::filesystem::path& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error) *error = "could not open " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return run_string(buffer.str(), path.string(), error);
}

std::size_t GameScriptHost::tick_listener_count() const noexcept { return impl_->tickRefs.size(); }
std::size_t GameScriptHost::damage_listener_count() const noexcept { return impl_->damageRefs.size(); }
std::size_t GameScriptHost::destroyed_listener_count() const noexcept { return impl_->destroyRefs.size(); }

std::optional<double> GameScriptHost::global_number(const std::string& key) const {
    const auto it = impl_->globals.find(key);
    if (it == impl_->globals.end()) return std::nullopt;
    return it->second;
}

std::optional<Float4> GameScriptHost::global_vector(const std::string& key) const {
    const auto it = impl_->globalVectors.find(key);
    if (it == impl_->globalVectors.end()) return std::nullopt;
    return it->second;
}

const ui::GameHudModel& GameScriptHost::hud_model() const { return impl_->hud; }
const MaterialLibrary& GameScriptHost::material_library() const { return impl_->materials; }
const MaterialParameterCollection& GameScriptHost::material_parameter_collection() const {
    return impl_->materials.parameter_collection();
}
const RenderEnvironment& GameScriptHost::environment() const { return impl_->environment; }

} // namespace dve
