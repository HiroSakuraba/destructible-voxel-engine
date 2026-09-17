// The actual point of this whole subsystem, end to end: a Lua gameplay script controlling
// objects simulated by real Jolt Physics through a real fixed-step tick loop, not a reference
// double and not a hand-written C++ scenario. This is what "scripting is done" should mean:
// a script author who has never seen this C++ codebase could write the Lua below.

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "dve/game_script.hpp"
#include "dve/physics_jolt_backend.hpp"

int main() {
    using namespace dve;

    GameWorld world(std::make_unique<JoltRigidBodyWorld>());
    GameScriptHost host(world);

    static constexpr const char* kScript = R"LUA(
        -- spawn_box only creates cubes (no separate width/height/depth), so size the "floor"
        -- to be small and position it so its top face sits at y=0, rather than a wide, tall
        -- block the crate could spawn buried inside of.
        floor = world.spawn_box("Floor", 8, 0.5, -2.0, -4.0, -2.0, false, 1000.0)
        crate = world.spawn_box("Crate", 4, 0.25, 0.0, 3.0, 0.0, true, 600.0)

        world.set_global("ticks", 0)
        world.set_global("launched", 0)
        world.set_global("destroyed", 0)

        world.on_tick(function(dt)
            world.set_global("ticks", world.get_global("ticks") + 1)
            local vel = world.get_velocity(crate)
            -- Once the crate has visibly settled (low velocity) and we haven't already
            -- launched it, give it a sideways kick. This is exactly the kind of "wait for a
            -- physical condition, then act" logic that is the whole reason to have a real
            -- tick loop instead of a single script invocation.
            if vel and world.get_global("launched") == 0
                and math.abs(vel.x) < 0.02 and math.abs(vel.y) < 0.02 and math.abs(vel.z) < 0.02 then
                world.apply_impulse(crate, 40.0, 2.0, 0.0)
                world.set_global("launched", 1)
                world.log("crate settled and launched")
            end
        end)

        world.on_destroyed(function(id)
            if id == crate then
                world.set_global("destroyed", 1)
                world.log("crate destroyed")
            end
        end)

        -- A raycast-driven "weapon": fire straight down at the crate's start position and log
        -- what it would have hit before the crate ever moves.
        local hit = world.raycast(0.0, 5.0, 0.0, 0.0, -1.0, 0.0, 20.0)
        if hit then
            world.log(string.format("raycast hit object %d at distance %.2f", hit.id, hit.distance))
            world.set_global("raycast_hit_id", hit.id)
        end

        -- After 3 seconds, blow the (by-then-airborne) crate apart.
        world.schedule_once(3.0, function()
            local pos = world.get_position(crate)
            if pos then
                world.log("detonating crate")
                world.damage_sphere(crate, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 5.0)
            end
        end)
    )LUA";

    std::string error;
    if (!host.run_string(kScript, "gameplay_demo", &error)) {
        std::fprintf(stderr, "script failed to run: %s\n", error.c_str());
        return EXIT_FAILURE;
    }

    constexpr float kFixedDelta = 1.0F / 120.0F;
    for (int step = 0; step < 120 * 6; ++step) world.tick(kFixedDelta);

    int failures = 0;
    auto check = [&](bool condition, const char* what) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };

    const auto ticks = host.global_number("ticks");
    check(ticks.has_value() && *ticks == 720.0, "on_tick should have fired exactly once per world.tick() call");
    const auto launched = host.global_number("launched");
    check(launched.has_value() && *launched == 1.0, "crate should have settled and been launched within 6 seconds");
    const auto destroyed = host.global_number("destroyed");
    check(destroyed.has_value() && *destroyed == 1.0, "scheduled detonation should have destroyed the crate");
    const auto raycastHitId = host.global_number("raycast_hit_id");
    check(raycastHitId.has_value(), "initial raycast should have found the crate before it moved");

    if (failures == 0) {
        std::printf("dve_game_script_jolt_smoke: PASS\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "dve_game_script_jolt_smoke: %d FAILURE(S)\n", failures);
    return EXIT_FAILURE;
}
