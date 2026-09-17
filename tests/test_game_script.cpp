#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include "dve/dvox.hpp"
#include "dve/camera_sequence.hpp"
#include "dve/game_script.hpp"
#include "dve/gameplay_runtime.hpp"

namespace {

int failures = 0;

#define CHECK(...)                                                                        \
    do {                                                                                  \
        if (!(__VA_ARGS__)) {                                                             \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n"; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

using namespace dve;

[[nodiscard]] bool run_ok(GameScriptHost& host, const std::string& code, const char* name) {
    std::string error;
    if (!host.run_string(code, name, &error)) {
        std::cerr << "lua error in " << name << ": " << error << "\n";
        return false;
    }
    return true;
}

void test_syntax_and_runtime_errors() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);

    std::string error;
    CHECK(!host.run_string("this is not lua {{{", "bad_syntax", &error));
    CHECK(!error.empty());

    error.clear();
    CHECK(!host.run_string("error('boom')", "bad_runtime", &error));
    CHECK(error.find("boom") != std::string::npos);

    // A prior failure must not corrupt the interpreter for later, valid scripts.
    error.clear();
    CHECK(host.run_string("world.log('still alive')", "ok", &error));
}

void test_spawn_and_position_roundtrip() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        id = world.spawn_box("Crate", 4, 0.25, 1.0, 5.0, 2.0, false, 1000.0)
        world.set_global("spawned_id", id)
        local pos = world.get_position(id)
        world.set_global("pos_x", pos.x)
        world.set_global("pos_y", pos.y)
        world.set_global("pos_z", pos.z)
    )", "spawn_test"));

    const auto id = host.global_number("spawned_id");
    CHECK(id.has_value());
    if (id) CHECK(world.has_object(static_cast<GameObjectId>(*id)));
    CHECK(host.global_number("pos_x") == 1.0);
    CHECK(host.global_number("pos_y") == 5.0);
    CHECK(host.global_number("pos_z") == 2.0);
}

void test_find_by_name_and_tag_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Turret";
    desc.tags = {"enemy", "structure"};
    desc.transform = make_rigid_transform({0.0F, 0.0F, 0.0F}, {});
    std::string setupError;
    const GameObjectId id = world.create_object(std::move(desc), &setupError);
    CHECK(id != kInvalidGameObjectId);

    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local found = world.find_by_name("Turret")
        world.set_global("found_by_name", found)
        local tagged = world.find_by_tag("enemy")
        world.set_global("tag_count", #tagged)
        world.set_global("tag_first", tagged[1])
        local missing = world.find_by_name("Nope")
        world.set_global("missing_is_nil", missing == nil and 1 or 0)
    )", "find_test"));

    CHECK(host.global_number("found_by_name") == static_cast<double>(id));
    CHECK(host.global_number("tag_count") == 1.0);
    CHECK(host.global_number("tag_first") == static_cast<double>(id));
    CHECK(host.global_number("missing_is_nil") == 1.0);
}


void test_component_composition_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Component Host";
    Component health;
    health.id = 1U;
    health.type = "game.health";
    health.properties.emplace("current", std::int64_t{75});
    health.properties.emplace("maximum", std::int64_t{100});
    desc.components.push_back(health);
    const GameObjectId id = world.create_object(std::move(desc));
    CHECK(id != kInvalidGameObjectId);

    GameScriptHost host(world);
    const std::string code = "host=" + std::to_string(id) + R"(
        local matches = world.find_by_component("game.health")
        world.set_global("component_match_count", #matches)
        world.set_global("component_match_id", matches[1])
        local list = world.get_components(host)
        world.set_global("component_count", #list)
        local health = world.get_component(host, 1)
        world.set_global("health_before", health.properties.current)
        local ok, err = world.set_component_property(host, 1, "current", 60)
        assert(ok, err)
        local inventory, add_err = world.add_component(host, "game.inventory", {
            capacity = 12,
            owner = "player",
            offset = {x=1.0, y=2.0, z=3.0}
        })
        assert(inventory, add_err)
        world.set_global("inventory_id", inventory)
        local inventory_data = world.get_component(host, inventory)
        world.set_global("inventory_capacity", inventory_data.properties.capacity)
        world.set_global("inventory_offset_y", inventory_data.properties.offset.y)
        world.set_global("removed_inventory", world.remove_component(host, inventory) and 1 or 0)
    )";
    CHECK(run_ok(host, code, "component_composition"));
    CHECK(host.global_number("component_match_count") == 1.0);
    CHECK(host.global_number("component_match_id") == static_cast<double>(id));
    CHECK(host.global_number("component_count") == 1.0);
    CHECK(host.global_number("health_before") == 75.0);
    CHECK(std::get<std::int64_t>(world.component(id, 1U)->properties.at("current")) == 60);
    CHECK(host.global_number("inventory_capacity") == 12.0);
    CHECK(host.global_number("inventory_offset_y") == 2.0);
    CHECK(host.global_number("removed_inventory") == 1.0);
    CHECK(world.find_by_component("game.inventory").empty());
}

void test_impulse_and_tick_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        id = world.spawn_box("Ball", 2, 0.5, 0.0, 0.0, 0.0, true, 1000.0)
        world.apply_impulse(id, 100.0, 0.0, 0.0)
        world.set_global("tick_count", 0)
        world.on_tick(function(dt)
            world.set_global("tick_count", world.get_global("tick_count") + 1)
            world.set_global("last_dt", dt)
        end)
    )", "impulse_test"));
    CHECK(host.tick_listener_count() == 1);

    for (int i = 0; i < 10; ++i) world.tick(1.0F / 60.0F);

    CHECK(host.global_number("tick_count") == 10.0);
    const auto lastDt = host.global_number("last_dt");
    CHECK(lastDt.has_value() && std::fabs(*lastDt - 1.0 / 60.0) < 1.0e-4);

    // The registered on_tick fired via GameWorld's own listener mechanism, and the impulse
    // should have actually moved the object: confirm through world.get_position too, not just
    // that the callback machinery ran.
    CHECK(run_ok(host, R"(
        local pos = world.get_position(id)
        world.set_global("moved", pos.x > 0.0 and 1 or 0)
    )", "check_moved"));
    CHECK(host.global_number("moved") == 1.0);
}

void test_timers_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        world.set_global("once_fired", 0)
        world.set_global("repeat_fired", 0)
        world.schedule_once(0.1, function()
            world.set_global("once_fired", world.get_global("once_fired") + 1)
        end)
        world.schedule_repeating(0.05, function()
            world.set_global("repeat_fired", world.get_global("repeat_fired") + 1)
        end)
    )", "timer_test"));

    for (int i = 0; i < 12; ++i) world.tick(1.0F / 60.0F); // ~0.2s

    CHECK(host.global_number("once_fired") == 1.0);
    const auto repeatFired = host.global_number("repeat_fired");
    CHECK(repeatFired.has_value() && *repeatFired >= 2.0);
}

void test_damage_and_destroy_events_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        id = world.spawn_box("Wall", 4, 1.0, 0.0, 0.0, 0.0, false, 1000.0)
        world.set_global("damage_events", 0)
        world.set_global("destroy_events", 0)
        world.on_damage(function(objectId, center, radius, removed, destroyed)
            world.set_global("damage_events", world.get_global("damage_events") + 1)
            world.set_global("last_removed", removed)
            world.set_global("last_damaged_id", objectId)
        end)
        world.on_destroyed(function(objectId)
            world.set_global("destroy_events", world.get_global("destroy_events") + 1)
            world.set_global("destroyed_id", objectId)
        end)
    )", "damage_setup"));

    CHECK(run_ok(host, "world.damage_sphere(id, 2.0, 2.0, 2.0, 1.2)", "damage1"));
    CHECK(host.global_number("damage_events") == 1.0);
    CHECK(host.global_number("destroy_events") == 0.0);
    const auto firstRemoved = host.global_number("last_removed");
    CHECK(firstRemoved.has_value() && *firstRemoved > 0.0);

    CHECK(run_ok(host, "world.damage_sphere(id, 2.0, 2.0, 2.0, 10.0)", "damage2"));
    CHECK(host.global_number("damage_events") == 2.0);
    CHECK(host.global_number("destroy_events") == 1.0);
}

void test_raycast_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local id = world.spawn_box("Target", 4, 0.5, 10.0, 0.0, 0.0, false, 1000.0)
        local hit = world.raycast(20.0, 1.0, 1.0, -1.0, 0.0, 0.0, 100.0)
        if hit then
            world.set_global("hit_id", hit.id)
            world.set_global("hit_x", hit.position.x)
            world.set_global("hit_distance", hit.distance)
        else
            world.set_global("hit_id", -1)
        end
        local miss = world.raycast(20.0, 50.0, 50.0, -1.0, 0.0, 0.0, 100.0)
        world.set_global("miss_is_nil", miss == nil and 1 or 0)
    )", "raycast_test"));

    CHECK(host.global_number("hit_id").has_value() && *host.global_number("hit_id") >= 0.0);
    const auto hitX = host.global_number("hit_x");
    CHECK(hitX.has_value() && std::fabs(*hitX - 12.0) < 1.0e-3);
    const auto hitDistance = host.global_number("hit_distance");
    CHECK(hitDistance.has_value() && std::fabs(*hitDistance - 8.0) < 1.0e-3);
    CHECK(host.global_number("miss_is_nil") == 1.0);
}

void test_rotation_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local id = world.spawn_box("Box", 2, 0.5, 0.0, 0.0, 0.0, true, 1000.0)
        world.set_rotation(id, 0.0, 90.0, 0.0)
        local rot = world.get_rotation(id)
        world.set_global("rot_y", rot.y)
    )", "rotation_test"));
    const auto rotY = host.global_number("rot_y");
    CHECK(rotY.has_value() && std::fabs(*rotY - 90.0) < 0.5);
}

void test_spawn_box3_non_cube_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local id = world.spawn_box3("Slab", 8, 2, 8, 0.5, -2.0, -1.0, -2.0, false, 1000.0)
        world.set_global("slab_id", id)
    )", "spawn3_test"));
    const auto id = host.global_number("slab_id");
    CHECK(id.has_value());
    if (id) CHECK(world.voxel_count(static_cast<GameObjectId>(*id)) == 8U * 2U * 8U);
}

void test_sphere_overlap_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        near = world.spawn_box("Near", 2, 1.0, 0.0, 0.0, 0.0, false, 1000.0)
        far = world.spawn_box("Far", 2, 1.0, 100.0, 100.0, 100.0, false, 1000.0)
        local hits = world.sphere_overlap(1.0, 1.0, 1.0, 3.0)
        world.set_global("hit_count", #hits)
        world.set_global("hit_is_near", hits[1] == near and 1 or 0)
    )", "overlap_test"));
    CHECK(host.global_number("hit_count") == 1.0);
    CHECK(host.global_number("hit_is_near") == 1.0);
}

void test_hud_binding_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        world.hud_set_interaction_prompt("Press E to open")
        world.hud_set_tools({
            {id="hammer", label="Hammer", enabled=true},
            {id="wrench", label="Wrench", enabled=true},
        })
    )", "hud_test"));
    CHECK(host.hud_model().interaction_prompt() == "Press E to open");
    const auto selected = host.hud_model().selected_tool();
    CHECK(selected.has_value() && selected->id == "hammer");
    CHECK(run_ok(host, "world.hud_select_next_tool()", "hud_next"));
    const auto afterNext = host.hud_model().selected_tool();
    CHECK(afterNext.has_value() && afterNext->id == "wrench");

    // A disabled tool must be skipped by cycling, not landed on.
    CHECK(run_ok(host, R"(
        world.hud_set_tools({
            {id="hammer", label="Hammer", enabled=true},
            {id="wrench", label="Wrench", enabled=false},
        })
        world.hud_select_next_tool()
    )", "hud_skip_disabled"));
    const auto afterSkip = host.hud_model().selected_tool();
    CHECK(afterSkip.has_value() && afterSkip->id == "hammer");
}

void test_input_state_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(!world.is_action_pressed("jump"));
    world.set_action_pressed("jump", true);
    world.set_axis("move_x", 0.75F);
    CHECK(run_ok(host, R"(
        world.set_global("jump_pressed", world.is_action_pressed("jump") and 1 or 0)
        world.set_global("move_x", world.get_axis("move_x"))
        world.set_action_pressed("crouch", true)
    )", "input_test"));
    CHECK(host.global_number("jump_pressed") == 1.0);
    CHECK(host.global_number("move_x") == 0.75);
    CHECK(world.is_action_pressed("crouch")); // set from Lua, read back from C++
}

void test_multiple_hosts_do_not_interfere() {
    GameWorld worldA(std::make_unique<ReferenceRigidBodyWorld>());
    GameWorld worldB(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost hostA(worldA);
    GameScriptHost hostB(worldB);

    CHECK(run_ok(hostA, R"(
        idA = world.spawn_box("OnlyInA", 2, 1.0, 0.0, 0.0, 0.0, false, 1000.0)
        world.set_global("marker", 111)
    )", "host_a_setup"));
    CHECK(run_ok(hostB, R"(
        idB = world.spawn_box("OnlyInB", 2, 1.0, 0.0, 0.0, 0.0, false, 1000.0)
        world.set_global("marker", 222)
    )", "host_b_setup"));

    CHECK(hostA.global_number("marker") == 111.0);
    CHECK(hostB.global_number("marker") == 222.0);
    CHECK(worldA.find_by_name("OnlyInA").has_value());
    CHECK(!worldA.find_by_name("OnlyInB").has_value());
    CHECK(worldB.find_by_name("OnlyInB").has_value());
    CHECK(!worldB.find_by_name("OnlyInA").has_value());
    CHECK(worldA.object_count() == 1);
    CHECK(worldB.object_count() == 1);
}

void test_spawn_asset_from_lua() {
    CookedVoxelAsset asset(555);
    asset.voxelSizeMeters = 0.25F;
    VoxelMaterialDefinition air;
    air.densityKilogramsPerCubicMeter = 0.0F;
    asset.materials.push_back(air);
    VoxelMaterialDefinition stone;
    stone.name = "Stone";
    stone.densityKilogramsPerCubicMeter = 2000.0F;
    asset.materials.push_back(stone);
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 3; ++z) asset.object.set_voxel({x, y, z}, 1);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "dve_lua_test_asset.dvox";
    std::string writeError;
    CHECK(write_dvox(path, asset, {}, &writeError));

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    const std::string script = "asset_id = world.spawn_asset(\"" + path.string() +
                                "\", 1.0, 2.0, 3.0, true, true)\n"
                                "world.set_global(\"asset_id\", asset_id)\n"
                                "local pos = world.get_position(asset_id)\n"
                                "world.set_global(\"asset_x\", pos.x)\n";
    CHECK(run_ok(host, script, "spawn_asset_test"));
    const auto id = host.global_number("asset_id");
    CHECK(id.has_value());
    if (id) CHECK(world.voxel_count(static_cast<GameObjectId>(*id)) == 27U);
    CHECK(host.global_number("asset_x") == 1.0);

    // A path that doesn't exist must fail cleanly with an error string, not a Lua error.
    CHECK(run_ok(host, R"(
        local id2, err = world.spawn_asset("/nonexistent/path.dvox", 0.0, 0.0, 0.0, true, true)
        world.set_global("missing_is_nil", id2 == nil and 1 or 0)
        world.set_global("has_error_string", type(err) == "string" and 1 or 0)
    )", "spawn_missing_asset"));
    CHECK(host.global_number("missing_is_nil") == 1.0);
    CHECK(host.global_number("has_error_string") == 1.0);

    std::filesystem::remove(path);
}

void test_fragmentation_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    // Same dumbbell shape as tests/test_game_world.cpp's C++-side fragmentation test, built
    // from Lua this time via repeated small spawn_box calls is not possible (spawn_box only
    // makes solid cubes/slabs), so this exercises fragmentation through world.damage_sphere
    // on a plain slab instead: cut a slab in half and confirm two independent pieces result.
    CHECK(run_ok(host, R"(
        slab = world.spawn_box3("Slab", 7, 1, 1, 1.0, 0.0, 0.0, 0.0, false, 1000.0)
        world.set_global("fragment_count", -1)
        world.on_damage(function(objectId, center, radius, removed, destroyed, fragments)
            world.set_global("fragment_count", #fragments)
            if #fragments > 0 then
                world.set_global("fragment_id", fragments[1])
            end
        end)
        world.damage_sphere(slab, 3.5, 0.5, 0.5, 0.6)
    )", "fragmentation_test"));
    CHECK(host.global_number("fragment_count") == 1.0);
    const auto fragmentId = host.global_number("fragment_id");
    CHECK(fragmentId.has_value());
    if (fragmentId) CHECK(world.has_object(static_cast<GameObjectId>(*fragmentId)));
}

void test_material_basic_and_overrides_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        masterId = world.create_master_material("Wood", {
            scalars = {
                {name="Roughness", default=0.8, min=0.0, max=1.0},
                {name="Metallic", default=0.0, min=0.0, max=1.0},
            },
            vectors = {
                {name="BaseColor", default={0.6,0.4,0.2,1.0}},
            },
        })
        world.set_global("master_ok", masterId ~= nil and 1 or 0)

        instId = world.create_material_instance({
            name = "DarkOak",
            material_id = 5,
            master = masterId,
            scalars = { Roughness = 0.5 },
            vectors = { BaseColor = {0.2, 0.1, 0.05, 1.0} },
            density = 700.0,
        })
        world.set_global("instance_ok", instId ~= nil and 1 or 0)

        local roughness = world.get_material_parameter(5, "Roughness")
        local metallic = world.get_material_parameter(5, "Metallic")
        local baseColor = world.get_material_parameter(5, "BaseColor")
        world.set_global("roughness", roughness)
        world.set_global("metallic", metallic)
        world.set_global("base_color_x", baseColor.x)
    )", "material_basic"));

    CHECK(host.global_number("master_ok") == 1.0);
    CHECK(host.global_number("instance_ok") == 1.0);
    const auto roughness = host.global_number("roughness");
    CHECK(roughness.has_value() && std::fabs(*roughness - 0.5) < 1.0e-4);
    const auto metallic = host.global_number("metallic");
    CHECK(metallic.has_value() && *metallic == 0.0); // not overridden: master default
    const auto baseColorX = host.global_number("base_color_x");
    CHECK(baseColorX.has_value() && std::fabs(*baseColorX - 0.2) < 1.0e-4);

    // The resolved VoxelMaterialDefinition on the C++ side must agree with what Lua saw.
    const VoxelMaterialDefinition* resolved = host.material_library().resolved(5);
    CHECK(resolved != nullptr);
    if (resolved) {
        CHECK(std::fabs(resolved->roughness - 0.5F) < 1.0e-4F);
        CHECK(std::fabs(resolved->densityKilogramsPerCubicMeter - 700.0F) < 1.0e-4F);
    }
}

void test_material_instance_chain_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local masterId = world.create_master_material("Metal", {
            scalars = { {name="Metallic", default=0.0, min=0.0, max=1.0}, {name="Roughness", default=0.8, min=0.0, max=1.0} },
            vectors = { {name="BaseColor", default={0.5,0.5,0.5,1.0}} },
        })
        local steelId = world.create_material_instance({
            name = "Steel", material_id = 10, master = masterId,
            scalars = { Metallic = 1.0, Roughness = 0.4 },
        })
        local rustyId = world.create_material_instance({
            name = "RustySteel", material_id = 11, parent = steelId,
            vectors = { BaseColor = {0.4, 0.2, 0.1, 1.0} },
        })
        world.create_material_instance({
            name = "PolishedRustySteel", material_id = 12, parent = rustyId,
            scalars = { Roughness = 0.1 },
        })
        world.set_global("rusty_metallic", world.get_material_parameter(11, "Metallic"))
        world.set_global("polished_roughness", world.get_material_parameter(12, "Roughness"))
        world.set_global("polished_base_x", world.get_material_parameter(12, "BaseColor").x)
    )", "material_chain"));

    const auto rustyMetallic = host.global_number("rusty_metallic");
    CHECK(rustyMetallic.has_value() && *rustyMetallic == 1.0); // inherited from grandparent Steel
    const auto polishedRoughness = host.global_number("polished_roughness");
    CHECK(polishedRoughness.has_value() && std::fabs(*polishedRoughness - 0.1) < 1.0e-4);
    const auto polishedBaseX = host.global_number("polished_base_x");
    CHECK(polishedBaseX.has_value() && std::fabs(*polishedBaseX - 0.4) < 1.0e-4); // inherited from parent RustySteel
}

void test_material_runtime_override_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local masterId = world.create_master_material("Lava", {
            scalars = { {name="Roughness", default=0.9, min=0.0, max=1.0} },
            vectors = { {name="Emissive", default={0,0,0,0}} },
        })
        world.create_material_instance({ name = "Lava01", material_id = 7, master = masterId })
        world.set_global("before", world.get_material_parameter(7, "Roughness"))
        local ok = world.set_material_parameter(7, "Roughness", 0.15)
        world.set_global("set_ok", ok and 1 or 0)
        world.set_global("after", world.get_material_parameter(7, "Roughness"))
        world.set_material_parameter(7, "Emissive", {2.0, 0.5, 0.0, 0.0})
        local emissive = world.get_material_parameter(7, "Emissive")
        world.set_global("emissive_x", emissive.x)
        local cleared = world.clear_material_parameters(7)
        world.set_global("cleared", cleared and 1 or 0)
        world.set_global("after_clear", world.get_material_parameter(7, "Roughness"))
    )", "material_runtime"));

    const auto before = host.global_number("before");
    CHECK(before.has_value() && std::fabs(*before - 0.9) < 1.0e-4);
    CHECK(host.global_number("set_ok") == 1.0);
    const auto after = host.global_number("after");
    CHECK(after.has_value() && std::fabs(*after - 0.15) < 1.0e-4);
    const auto emissiveX = host.global_number("emissive_x");
    CHECK(emissiveX.has_value() && std::fabs(*emissiveX - 2.0) < 1.0e-4);
    CHECK(host.global_number("cleared") == 1.0);
    const auto afterClear = host.global_number("after_clear");
    CHECK(afterClear.has_value() && std::fabs(*afterClear - 0.9) < 1.0e-4); // back to master default
}

void test_material_error_paths_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local badMaster, err1 = world.create_master_material("", {})
        world.set_global("empty_name_rejected", badMaster == nil and 1 or 0)
        world.set_global("empty_name_has_error", type(err1) == "string" and 1 or 0)

        local masterId = world.create_master_material("M", {
            scalars = { {name="Roughness", default=0.5, min=0.0, max=1.0} },
        })
        world.create_material_instance({ name = "A", material_id = 20, master = masterId })

        local dupInst, err2 = world.create_material_instance({ name = "B", material_id = 20, master = masterId })
        world.set_global("duplicate_material_id_rejected", dupInst == nil and 1 or 0)
        world.set_global("duplicate_has_error", type(err2) == "string" and 1 or 0)

        local setOk, err3 = world.set_material_parameter(20, "NotAParam", 0.5)
        world.set_global("unknown_param_rejected", setOk == false and 1 or 0)
        world.set_global("unknown_param_has_error", type(err3) == "string" and 1 or 0)

        local rangeOk = world.set_material_parameter(20, "Roughness", 99.0)
        world.set_global("out_of_range_rejected", rangeOk == false and 1 or 0)

        local missing = world.get_material_parameter(250, "Roughness")
        world.set_global("missing_material_is_nil", missing == nil and 1 or 0)
    )", "material_errors"));

    CHECK(host.global_number("empty_name_rejected") == 1.0);
    CHECK(host.global_number("empty_name_has_error") == 1.0);
    CHECK(host.global_number("duplicate_material_id_rejected") == 1.0);
    CHECK(host.global_number("duplicate_has_error") == 1.0);
    CHECK(host.global_number("unknown_param_rejected") == 1.0);
    CHECK(host.global_number("unknown_param_has_error") == 1.0);
    CHECK(host.global_number("out_of_range_rejected") == 1.0);
    CHECK(host.global_number("missing_material_is_nil") == 1.0);
}

void test_unreal_style_material_globals_layers_and_shading_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local rustMaster = world.create_master_material("RustLayer", {
            shading_model = "standard_pbr",
            scalars = {
                {name="Metallic", default=0.1, min=0.0, max=1.0},
                {name="Roughness", default=0.9, min=0.0, max=1.0},
                {name="Specular", default=0.35, min=0.0, max=1.0}
            },
            vectors = {
                {name="BaseColor", default={0.6, 0.1, 0.02, 1.0}}
            }
        })
        local rust = world.create_material_instance({
            name="Rust", material_id=60, master=rustMaster
        })

        local coatMaster = world.create_master_material("PaintedClearCoat", {
            shading_model = "clear_coat",
            scalars = {
                {name="Metallic", default=0.8, min=0.0, max=1.0},
                {name="Roughness", default=0.4, min=0.0, max=1.0},
                {name="Specular", default=0.5, min=0.0, max=1.0},
                {name="ClearCoat", default=0.8, min=0.0, max=1.0},
                {name="ClearCoatRoughness", default=0.12, min=0.0, max=1.0}
            },
            vectors = {
                {name="BaseColor", default={0.8, 0.8, 0.8, 1.0}}
            },
            global_scalars = {
                {parameter="Roughness", global="RoughnessScale", combine="multiply"}
            },
            global_vectors = {
                {parameter="BaseColor", global="TimeOfDayTint", combine="multiply"}
            }
        })
        local coated = world.create_material_instance({
            name="RustOverPaint", material_id=61, master=coatMaster,
            layers={{material_id=60, weight=0.25, blend="lerp"}}
        })

        local scalarOk = world.set_global("RoughnessScale", 0.5)
        local vectorOk = world.set_global_vector("TimeOfDayTint", 0.5, 1.0, 0.75, 1.0)
        local roughness = world.get_material_parameter(61, "Roughness")
        local base = world.get_material_parameter(61, "BaseColor")
        local coat = world.get_material_parameter(61, "ClearCoat")
        local weightBefore = world.get_material_layer_weight(61, 1)
        local layerOk = world.set_material_layer_weight(61, 1, 0.5)
        local weightAfter = world.get_material_layer_weight(61, 1)
        local roughnessAfter = world.get_material_parameter(61, "Roughness")

        local badGlobal, badError = world.set_global("RoughnessScale", 3.0)
        local roughnessScaleAfterReject = world.get_global("RoughnessScale")

        world.set_global("material_ids_created", rust ~= nil and coated ~= nil and 1 or 0)
        world.set_global("material_globals_ok", scalarOk and vectorOk and 1 or 0)
        world.set_global("material_roughness", roughness)
        world.set_global("material_base_x", base.x)
        world.set_global("material_clear_coat", coat)
        world.set_global("layer_weight_before", weightBefore)
        world.set_global("layer_weight_after", weightAfter)
        world.set_global("layer_update_ok", layerOk and 1 or 0)
        world.set_global("roughness_after_layer", roughnessAfter)
        world.set_global("bad_global_rejected", badGlobal == false and type(badError) == "string" and 1 or 0)
        world.set_global("global_rollback_value", roughnessScaleAfterReject)
    )", "unreal_material_features"));

    CHECK(host.global_number("material_ids_created") == 1.0);
    CHECK(host.global_number("material_globals_ok") == 1.0);
    CHECK(std::fabs(*host.global_number("material_roughness") - 0.375) < 1.0e-5);
    CHECK(std::fabs(*host.global_number("material_base_x") - 0.45) < 1.0e-5);
    CHECK(std::fabs(*host.global_number("material_clear_coat") - 0.6) < 1.0e-5);
    CHECK(std::fabs(*host.global_number("layer_weight_before") - 0.25) < 1.0e-5);
    CHECK(std::fabs(*host.global_number("layer_weight_after") - 0.5) < 1.0e-5);
    CHECK(host.global_number("layer_update_ok") == 1.0);
    CHECK(std::fabs(*host.global_number("roughness_after_layer") - 0.55) < 1.0e-5);
    CHECK(host.global_number("bad_global_rejected") == 1.0);
    CHECK(std::fabs(*host.global_number("global_rollback_value") - 0.5) < 1.0e-5);

    const VoxelMaterialDefinition* resolved = host.material_library().resolved(61);
    CHECK(resolved != nullptr);
    if (resolved) {
        CHECK(resolved->shadingModel == MaterialShadingModel::ClearCoat);
        CHECK(std::fabs(resolved->clearCoat - 0.4F) < 1.0e-5F);
        CHECK(resolved->layers.size() == 1);
    }
    CHECK(host.material_parameter_collection().scalar_slot("RoughnessScale").has_value());
    CHECK(host.material_parameter_collection().vector_slot("TimeOfDayTint") == kMpcTimeOfDayTintSlot);
}

void test_environment_bindings_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local ok1 = world.set_environment_scalar("Exposure", 2.5)
        world.set_global("exposure_ok", ok1 and 1 or 0)
        local ok2 = world.set_environment_vector("SunColor", 1.0, 0.8, 0.6)
        world.set_global("sun_color_ok", ok2 and 1 or 0)
        local ok3 = world.set_environment_tonemap_operator("reinhard")
        world.set_global("tonemap_ok", ok3 and 1 or 0)
        local ok4 = world.set_environment_gi_mode("ambient")
        local ok5 = world.set_environment_shadow_mode("hybrid")
        local ok6 = world.set_environment_scalar("GlobalIlluminationIntensity", 0.8)
        local ok7 = world.set_environment_scalar("ShadowSamples", 6)
        world.set_global("lighting_modes_ok", ok4 and ok5 and ok6 and ok7 and 1 or 0)

        local env = world.get_environment()
        world.set_global("exposure_readback", env.exposure)
        world.set_global("sun_color_r", env.sun_color.x)
        world.set_global("tonemap_readback_is_reinhard", env.tonemap_operator == "reinhard" and 1 or 0)
        world.set_global("gi_readback_is_ambient", env.global_illumination_mode == "ambient" and 1 or 0)
        world.set_global("shadow_readback_is_hybrid", env.shadow_mode == "hybrid" and 1 or 0)
        world.set_global("shadow_sample_readback", env.shadow_samples)
    )", "environment_basic"));

    CHECK(host.global_number("exposure_ok") == 1.0);
    CHECK(host.global_number("sun_color_ok") == 1.0);
    CHECK(host.global_number("tonemap_ok") == 1.0);
    CHECK(host.global_number("lighting_modes_ok") == 1.0);
    const auto exposure = host.global_number("exposure_readback");
    CHECK(exposure.has_value() && std::fabs(*exposure - 2.5) < 1.0e-4);
    const auto sunColorR = host.global_number("sun_color_r");
    CHECK(sunColorR.has_value() && std::fabs(*sunColorR - 1.0) < 1.0e-4);
    CHECK(host.global_number("tonemap_readback_is_reinhard") == 1.0);
    CHECK(host.global_number("gi_readback_is_ambient") == 1.0);
    CHECK(host.global_number("shadow_readback_is_hybrid") == 1.0);
    CHECK(host.global_number("shadow_sample_readback") == 6.0);

    // Cross-check against the C++-side accessor directly, not just what Lua reported back.
    CHECK(std::fabs(host.environment().exposure - 2.5F) < 1.0e-4F);
    CHECK(host.environment().tonemapOperator == TonemapOperator::Reinhard);
    CHECK(host.environment().globalIlluminationMode == GlobalIlluminationMode::AmbientHemisphere);
    CHECK(host.environment().shadowMode == ShadowMode::Hybrid);
    CHECK(host.environment().shadowSamples == 6U);
}


void test_camera_runtime_bindings_from_lua() {
    using namespace dve::camera;
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::string error;
    CHECK(world.cameras().add_viewport({1,"Main",1,0,0,1,1,"main",true},&error));
    CameraRig primary;primary.id=10;primary.name="Primary";primary.priority=10;primary.authoredPose.position={0,1,6};primary.authoredPose.target={0,1,0};primary.collision.enabled=false;
    CameraRig alternate=primary;alternate.id=11;alternate.name="Alternate";alternate.priority=1;alternate.authoredPose.position={3,2,5};
    auto* director=world.cameras().director(1);CHECK(director!=nullptr);CHECK(director->add_or_replace_rig(primary,&error));CHECK(director->add_or_replace_rig(alternate,&error));director->bind_state("alternate",11);
    CameraSequence sequence;sequence.name="Lua Camera";sequence.durationSeconds=2;CameraShot shot;shot.id=1;shot.name="Primary shot";shot.startSeconds=0;shot.durationSeconds=2;shot.rigId=10;shot.blendIn={CameraBlendCurve::Cut,0};sequence.shots.push_back(shot);sequence.events.push_back({1,0.25F,"cue","camera"});
    const auto base=std::filesystem::temp_directory_path()/"dve_camera_lua_test";std::filesystem::create_directories(base);const auto sequencePath=base/"camera.dvecamseq";const auto statePath=base/"camera.state";{std::ofstream output(sequencePath,std::ios::binary|std::ios::trunc);output<<sequence.serialize();}
    GameScriptHost host(world);
    const std::string setup="local ok,err=world.camera_load_sequence(1,"+std::string("\"")+sequencePath.string()+"\")\nworld.set_global('camera_load_ok',ok and 1 or 0)\nworld.camera_play_sequence(1,false)\nworld.camera_start_shake(1,77,0.01,0.2,0.5,8)\nlocal access=world.camera_set_accessibility('photosensitive',true,0.5,1.0,1.0,0.8)\nlocal a=world.camera_get_accessibility()\nworld.set_global('camera_access_ok',access and a.preset=='photosensitive' and a.horizon_lock and 1 or 0)";
    CHECK(run_ok(host,setup,"camera_setup"));
    world.tick(0.3F);
    const std::string inspect="local c=world.camera_get(1)\nworld.set_global('camera_time',c.sequence_time)\nworld.set_global('camera_event_count',#c.events)\nworld.set_global('camera_force_ok',world.camera_force_live(1,11,true) and 1 or 0)\nworld.set_global('camera_state_ok',world.camera_set_state(1,'alternate') and 1 or 0)\nlocal ok=world.camera_save_runtime_state("+std::string("\"")+statePath.string()+"\")\nworld.set_global('camera_save_ok',ok and 1 or 0)";
    CHECK(run_ok(host,inspect,"camera_inspect"));
    CHECK(host.global_number("camera_load_ok")==1.0);CHECK(host.global_number("camera_access_ok")==1.0);CHECK(host.global_number("camera_event_count")==1.0);CHECK(host.global_number("camera_force_ok")==1.0);CHECK(host.global_number("camera_state_ok")==1.0);CHECK(host.global_number("camera_save_ok")==1.0);CHECK(host.global_number("camera_time").value_or(0.0)>0.25);
    CHECK(run_ok(host,"world.camera_seek_sequence(1,1.5)\nworld.camera_stop_sequence(1)\nlocal ok=world.camera_load_runtime_state("+std::string("\"")+statePath.string()+"\")\nworld.set_global('camera_restore_ok',ok and 1 or 0)","camera_restore"));
    CHECK(host.global_number("camera_restore_ok")==1.0);CHECK(world.cameras().sequence_time(1)<0.5F);CHECK(world.cameras().sequence_playing(1));
    std::filesystem::remove_all(base);
}


void test_playable_runtime_bindings_from_lua() {
    auto physics = std::make_unique<ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    GameWorld world(std::move(physics));
    GameObjectDesc pawnDesc;
    pawnDesc.name = "Lua Pawn";
    pawnDesc.tags = {"player"};
    pawnDesc.transform.position = {0,0,0.25F};
    const GameObjectId pawn = world.create_object(std::move(pawnDesc));
    CHECK(pawn != kInvalidGameObjectId);
    GameObjectDesc floorDesc;
    floorDesc.name = "Floor";
    floorDesc.voxelSizeMeters = 0.25F;
    floorDesc.transform.position = {-5,-2,0};
    floorDesc.dynamic = false;
    floorDesc.voxels = std::make_unique<VoxelObject>(1);
    for (int y=0;y<16;++y) for(int x=0;x<40;++x) floorDesc.voxels->set_voxel({x,y,0},1);
    CHECK(world.create_object(std::move(floorDesc)) != kInvalidGameObjectId);

    GameScriptHost host(world);
    const std::string setup = "pawn=" + std::to_string(pawn) + R"(
        local ok, err = world.character_add(pawn, {speed=4.0, jump_speed=6.5, step_height=0.3})
        assert(ok, err)
        player = world.player_create("Lua Player", true)
        local possessed, possess_err = world.player_possess(player, pawn)
        assert(possessed, possess_err)
        trigger = world.trigger_create_box("Lua Trigger", 0.75,0.0,1.0, 0.5,1.0,1.5, false, "player")
        world.set_global("trigger_enter", 0)
        world.on_trigger(function(id, kind, object_id, fixed_tick)
            if id == trigger and kind == "enter" then
                world.set_global("trigger_enter", world.get_global("trigger_enter") + 1)
                world.set_global("trigger_object", object_id)
                world.set_global("trigger_tick", fixed_tick)
            end
        end)
        world.player_set_input(player, 1.0, 0.0, false, false)
    )";
    CHECK(run_ok(host, setup, "playable_runtime_setup"));
    for (int index=0; index<40; ++index) world.tick(1.0F/60.0F);
    CHECK(host.global_number("trigger_enter").value_or(0.0) >= 1.0);
    CHECK(host.global_number("trigger_object") == static_cast<double>(pawn));
    CHECK(host.global_number("trigger_tick").value_or(0.0) > 0.0);
    CHECK(run_ok(host, R"(
        local c = world.character_get(pawn)
        world.set_global("character_grounded", c.grounded and 1 or 0)
        world.set_global("character_x", world.get_position(pawn).x)
        world.player_set_input(player, 0.0, 0.0, true, false)
    )", "playable_runtime_inspect"));
    CHECK(host.global_number("character_grounded") == 1.0);
    CHECK(host.global_number("character_x").value_or(0.0) > 0.25);
    world.tick(1.0F/60.0F);
    CHECK(world.gameplay().character(pawn)->velocity.z > 0.0F);
}

void test_environment_validation_rejects_and_rolls_back_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    CHECK(run_ok(host, "world.set_environment_scalar(\"Exposure\", 3.0)", "environment_setup"));
    CHECK(std::fabs(host.environment().exposure - 3.0F) < 1.0e-4F);

    CHECK(run_ok(host, R"(
        local ok, err = world.set_environment_scalar("Exposure", -1.0)
        world.set_global("negative_exposure_rejected", ok == false and 1 or 0)
        world.set_global("has_error", type(err) == "string" and 1 or 0)

        local ok2, err2 = world.set_environment_scalar("NotAField", 1.0)
        world.set_global("unknown_field_rejected", ok2 == false and 1 or 0)
        world.set_global("unknown_has_error", type(err2) == "string" and 1 or 0)

        local ok3 = world.set_environment_tonemap_operator("nonsense")
        world.set_global("bad_tonemap_rejected", ok3 == false and 1 or 0)
        local ok4 = world.set_environment_gi_mode("nonsense")
        world.set_global("bad_gi_rejected", ok4 == false and 1 or 0)
        local ok5 = world.set_environment_shadow_mode("nonsense")
        world.set_global("bad_shadow_rejected", ok5 == false and 1 or 0)
    )", "environment_rejection"));

    CHECK(host.global_number("negative_exposure_rejected") == 1.0);
    CHECK(host.global_number("has_error") == 1.0);
    CHECK(host.global_number("unknown_field_rejected") == 1.0);
    CHECK(host.global_number("unknown_has_error") == 1.0);
    CHECK(host.global_number("bad_tonemap_rejected") == 1.0);
    CHECK(host.global_number("bad_gi_rejected") == 1.0);
    CHECK(host.global_number("bad_shadow_rejected") == 1.0);

    // The rejected write must not have left exposure half-applied: still the last *valid*
    // value (3.0), not -1.0 and not some other partial state.
    CHECK(std::fabs(host.environment().exposure - 3.0F) < 1.0e-4F);
}

void test_membership_lifecycle_and_pooling_from_lua() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc fixture;
    fixture.name = "Grouped Fixture";
    fixture.tags = {"target"};
    fixture.groups = {"mission"};
    fixture.layer = 9U;
    const GameObjectId fixtureId = world.create_object(std::move(fixture));
    CHECK(fixtureId != kInvalidGameObjectId);

    GameScriptHost host(world);
    CHECK(run_ok(host, R"(
        local grouped = world.find_by_group("mission")
        local layered = world.find_by_layer(9)
        world.set_global("group_count", #grouped)
        world.set_global("group_first", grouped[1])
        world.set_global("layer_count", #layered)
        world.set_global("lifecycle_count", 0)
        world.on_lifecycle(function(kind, object_id, other_id, component_id, component_type, pool_name)
            world.set_global("lifecycle_count", world.get_global("lifecycle_count") + 1)
            if kind == "pool_acquire" then
                world.set_global("saw_pool_acquire", 1)
                world.set_global("pool_object", object_id)
                world.set_global("pool_name_ok", pool_name == "lua_projectiles" and 1 or 0)
            elseif kind == "pool_release" then
                world.set_global("saw_pool_release", 1)
            elseif kind == "component_disabled" then
                world.set_global("saw_component_disabled", component_type == "game.lua_probe" and 1 or 0)
            end
        end)
        local component, component_err = world.add_component(grouped[1], "game.lua_probe", {active=true})
        assert(component, component_err)
        assert(world.set_component_enabled(grouped[1], component, false))
        assert(world.set_enabled(grouped[1], false))
        world.set_global("disabled_hidden", #world.find_by_group("mission") == 0 and 1 or 0)
        assert(world.set_enabled(grouped[1], true))

        local pool, pool_err = world.pool_create_box("lua_projectiles", 2, 1, 1, 1, 0.25, false, 1000.0)
        assert(pool, pool_err)
        world.set_global("pool_initial", world.pool_available(pool))
        local first, acquire_err = world.pool_acquire(pool, 1.0, 2.0, 3.0)
        assert(first, acquire_err)
        world.set_global("pool_after_acquire", world.pool_available(pool))
        local ok, release_err = world.pool_release(first)
        assert(ok, release_err)
        world.set_global("pool_after_release", world.pool_available(pool))
        local recycled = world.pool_acquire(pool, 4.0, 5.0, 6.0)
        world.set_global("pool_recycled_same", recycled == first and 1 or 0)
    )", "membership_lifecycle_pool"));
    CHECK(host.global_number("group_count") == 1.0);
    CHECK(host.global_number("group_first") == static_cast<double>(fixtureId));
    CHECK(host.global_number("layer_count") == 1.0);
    CHECK(host.global_number("disabled_hidden") == 1.0);
    CHECK(host.global_number("saw_component_disabled") == 1.0);
    CHECK(host.global_number("pool_initial") == 2.0);
    CHECK(host.global_number("pool_after_acquire") == 1.0);
    CHECK(host.global_number("pool_after_release") == 2.0);
    CHECK(host.global_number("pool_recycled_same") == 1.0);
    CHECK(host.global_number("saw_pool_acquire") == 1.0);
    CHECK(host.global_number("saw_pool_release") == 1.0);
    CHECK(host.global_number("pool_name_ok") == 1.0);
}


} // namespace

int main() {
    test_syntax_and_runtime_errors();
    test_spawn_and_position_roundtrip();
    test_find_by_name_and_tag_from_lua();
    test_component_composition_from_lua();
    test_impulse_and_tick_from_lua();
    test_timers_from_lua();
    test_damage_and_destroy_events_from_lua();
    test_raycast_from_lua();
    test_rotation_from_lua();
    test_spawn_box3_non_cube_from_lua();
    test_sphere_overlap_from_lua();
    test_hud_binding_from_lua();
    test_input_state_from_lua();
    test_multiple_hosts_do_not_interfere();
    test_spawn_asset_from_lua();
    test_fragmentation_from_lua();
    test_material_basic_and_overrides_from_lua();
    test_material_instance_chain_from_lua();
    test_material_runtime_override_from_lua();
    test_material_error_paths_from_lua();
    test_unreal_style_material_globals_layers_and_shading_from_lua();
    test_environment_bindings_from_lua();
    test_camera_runtime_bindings_from_lua();
    test_environment_validation_rejects_and_rolls_back_from_lua();
    test_playable_runtime_bindings_from_lua();
    test_membership_lifecycle_and_pooling_from_lua();

    if (failures == 0) {
        std::cout << "dve_game_script_tests: PASS\n";
        return 0;
    }
    std::cerr << "dve_game_script_tests: " << failures << " FAILURE(S)\n";
    return 1;
}
