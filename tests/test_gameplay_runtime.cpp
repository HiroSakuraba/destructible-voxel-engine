#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/gameplay_runtime.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

std::unique_ptr<dve::VoxelObject> solid_box(int sx, int sy, int sz, dve::MaterialId material = 1) {
    auto voxels = std::make_unique<dve::VoxelObject>(material);
    for (int z = 0; z < sz; ++z) for (int y = 0; y < sy; ++y) for (int x = 0; x < sx; ++x)
        voxels->set_voxel({x,y,z}, material);
    return voxels;
}

dve::GameObjectId add_box(dve::GameWorld& world, const char* name, int sx, int sy, int sz,
                          float voxel, dve::Float3 position, bool dynamic = false,
                          std::vector<std::string> tags = {}) {
    dve::GameObjectDesc desc;
    desc.name = name;
    desc.tags = std::move(tags);
    desc.voxelSizeMeters = voxel;
    desc.transform = dve::make_rigid_transform(position, {});
    desc.voxels = solid_box(sx, sy, sz);
    desc.dynamic = dynamic;
    std::string error;
    const auto id = world.create_object(std::move(desc), &error);
    require(id != dve::kInvalidGameObjectId, error.c_str());
    return id;
}

dve::GameObjectId add_marker(dve::GameWorld& world, const char* name, dve::Float3 position,
                             std::vector<std::string> tags = {}) {
    dve::GameObjectDesc desc;
    desc.name = name;
    desc.tags = std::move(tags);
    desc.transform = dve::make_rigid_transform(position, {});
    const auto id = world.create_object(std::move(desc));
    require(id != dve::kInvalidGameObjectId, "marker creation failed");
    return id;
}

struct Fixture {
    std::unique_ptr<dve::ReferenceRigidBodyWorld> physics;
    std::unique_ptr<dve::GameWorld> world;
    dve::GameObjectId floor{};
    dve::GameObjectId pawn{};
    dve::GamePlayerId player{};
};

Fixture make_fixture() {
    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    auto* physicsPointer = physics.get();
    auto world = std::make_unique<dve::GameWorld>(std::move(physics));
    (void)physicsPointer;
    const auto floor = add_box(*world, "Floor", 40, 40, 1, 0.25F, {-5,-5,0}, false, {"floor"});
    add_box(*world, "Step", 4, 6, 1, 0.25F, {1.5F,-0.75F,0.25F});
    add_box(*world, "Wall", 1, 12, 12, 0.25F, {4.0F,-1.5F,0.25F});
    const auto pawn = add_marker(*world, "Pawn", {0,0,0.25F}, {"player"});
    std::string error;
    require(world->gameplay().add_character(pawn, {}, &error), error.c_str());
    const auto player = world->gameplay().create_player("Local", true);
    require(world->gameplay().possess(player, pawn, &error), error.c_str());
    return {nullptr, std::move(world), floor, pawn, player};
}

void tick(dve::GameWorld& world, int count, float dt = 1.0F / 60.0F) {
    for (int i = 0; i < count; ++i) world.tick(dt);
}

void test_walk_step_wall_jump_crouch() {
    auto fixture = make_fixture();
    auto& world = *fixture.world;
    tick(world, 3);
    const auto* initial = world.gameplay().character(fixture.pawn);
    require(initial && initial->grounded, "character did not settle onto the floor");

    world.gameplay().set_player_input(fixture.player, {{1,0,0}, false, false});
    float maximumHeight = world.position(fixture.pawn)->z;
    bool stepped = false;
    for (int i = 0; i < 45; ++i) {
        world.tick(1.0F / 60.0F);
        maximumHeight = std::max(maximumHeight, world.position(fixture.pawn)->z);
        stepped = stepped || world.gameplay().telemetry(fixture.pawn)->successfulSteps > 0U;
    }
    const auto position = world.position(fixture.pawn).value();
    require(position.x > 1.0F, "character did not walk");
    require(stepped || maximumHeight > 0.40F, "step-up path was not exercised");

    tick(world, 120);
    const auto atWall = world.position(fixture.pawn).value();
    require(atWall.x < 4.0F, "character crossed the wall");

    world.gameplay().set_player_input(fixture.player, {{0,0,0}, true, false});
    world.tick(1.0F/60.0F);
    const auto* jumping = world.gameplay().character(fixture.pawn);
    require(jumping && !jumping->grounded && jumping->velocity.z > 0.0F, "jump did not launch");
    const float jumpStart = world.position(fixture.pawn)->z;
    tick(world, 12);
    require(world.position(fixture.pawn)->z > jumpStart, "jump did not raise the pawn");

    world.gameplay().set_player_input(fixture.player, {{0,0,0}, false, true});
    tick(world, 1);
    require(world.gameplay().character(fixture.pawn)->stance == dve::CharacterStance::Crouched,
            "crouch stance was not entered");
}

void test_trigger_enter_stay_exit_and_state() {
    auto fixture = make_fixture();
    auto& world = *fixture.world;
    dve::TriggerVolumeDesc trigger;
    trigger.name = "Checkpoint";
    trigger.transform.position = {1.0F, 0.0F, 1.0F};
    trigger.halfExtents = {0.8F, 1.0F, 1.5F};
    trigger.requiredTag = "player";
    std::string error;
    const auto triggerId = world.gameplay().create_trigger(trigger, &error);
    require(triggerId != dve::kInvalidGameTriggerId, error.c_str());
    std::vector<dve::TriggerEventKind> events;
    world.gameplay().on_trigger([&](const dve::TriggerEvent& event) {
        if (event.trigger == triggerId) events.push_back(event.kind);
    });
    world.gameplay().set_player_input(fixture.player, {{1,0,0},false,false});
    tick(world, 30);
    world.gameplay().set_player_input(fixture.player, {{0,0,0},false,false});
    tick(world, 2);
    world.gameplay().set_player_input(fixture.player, {{-1,0,0},false,false});
    tick(world, 60);
    require(std::find(events.begin(), events.end(), dve::TriggerEventKind::Enter) != events.end(),
            "trigger enter was not emitted");
    require(std::find(events.begin(), events.end(), dve::TriggerEventKind::Stay) != events.end(),
            "trigger stay was not emitted");
    require(std::find(events.begin(), events.end(), dve::TriggerEventKind::Exit) != events.end(),
            "trigger exit was not emitted");
    const std::string state = world.gameplay().serialize_trigger_state();
    require(world.gameplay().set_trigger_enabled(triggerId, false), "trigger disable failed");
    require(world.gameplay().restore_trigger_state(state, &error), error.c_str());
    require(world.gameplay().trigger(triggerId)->enabled, "trigger state restore failed");
}

void test_floor_removal_and_replay() {
    auto fixture = make_fixture();
    auto& world = *fixture.world;
    tick(world, 3);
    require(world.gameplay().character(fixture.pawn)->grounded, "pawn was not grounded before floor removal");
    require(world.destroy_object(fixture.floor), "floor destroy failed");
    world.tick(1.0F/60.0F);
    require(world.gameplay().telemetry(fixture.pawn)->floorRemoved, "floor removal was not detected");
    require(!world.gameplay().character(fixture.pawn)->grounded, "pawn remained grounded without a floor");

    auto a = make_fixture();
    require(a.world->gameplay().begin_recording(a.player), "recording did not start");
    for (int i = 0; i < 90; ++i) {
        a.world->gameplay().set_player_input(a.player, {{i < 60 ? 0.6F : -0.2F, 0.35F, 0}, i == 20, i > 70});
        a.world->tick(1.0F/60.0F);
    }
    const auto replay = a.world->gameplay().end_recording(a.player);
    require(replay && replay->frames.size() == 90U, "recording frame count is wrong");
    auto b = make_fixture();
    require(b.world->gameplay().begin_playback(b.player, *replay), "playback did not start");
    tick(*b.world, 90);
    const auto pa = a.world->position(a.pawn).value();
    const auto pb = b.world->position(b.pawn).value();
    require(dve::length(dve::subtract(pa,pb)) < 1.0e-4F, "deterministic replay diverged");
    const auto* sa = a.world->gameplay().character(a.pawn);
    const auto* sb = b.world->gameplay().character(b.pawn);
    require(dve::length(dve::subtract(sa->velocity,sb->velocity)) < 1.0e-4F,
            "replay velocity diverged");
}


void test_jump_windows_crouch_clearance_and_possession() {
    auto coyote = make_fixture();
    tick(*coyote.world, 3);
    require(coyote.world->gameplay().character(coyote.pawn)->grounded, "coyote fixture did not ground");
    coyote.world->destroy_object(coyote.floor);
    coyote.world->tick(1.0F/60.0F);
    coyote.world->gameplay().set_player_input(coyote.player, {{0,0,0},true,false});
    coyote.world->tick(1.0F/60.0F);
    require(coyote.world->gameplay().character(coyote.pawn)->velocity.z > 0.0F,
            "coyote-time jump was not accepted after leaving the floor");

    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    auto buffered = std::make_unique<dve::GameWorld>(std::move(physics));
    add_box(*buffered, "Floor", 20,20,1,0.25F,{-2.5F,-2.5F,0});
    const auto fallingPawn = add_marker(*buffered, "Falling Pawn", {0,0,0.55F});
    std::string error;
    require(buffered->gameplay().add_character(fallingPawn, {}, &error), error.c_str());
    auto* fallingState = buffered->gameplay().character(fallingPawn);
    fallingState->velocity.z = -4.0F;
    buffered->gameplay().set_character_input(fallingPawn, {{0,0,0},true,false});
    bool bufferedJumped = false;
    for (int i=0;i<12;++i) {
        buffered->tick(1.0F/60.0F);
        bufferedJumped = bufferedJumped || buffered->gameplay().telemetry(fallingPawn)->jumped;
    }
    require(bufferedJumped && buffered->gameplay().character(fallingPawn)->velocity.z > 0.0F,
            "buffered jump did not fire on landing");

    auto crouch = make_fixture();
    tick(*crouch.world, 3);
    crouch.world->gameplay().set_player_input(crouch.player, {{0,0,0},false,true});
    crouch.world->tick(1.0F/60.0F);
    require(crouch.world->gameplay().character(crouch.pawn)->stance == dve::CharacterStance::Crouched,
            "crouch fixture did not crouch");
    add_box(*crouch.world, "Low Ceiling", 6,6,1,0.25F,{-0.75F,-0.75F,1.40F});
    crouch.world->gameplay().set_player_input(crouch.player, {{0,0,0},false,false});
    crouch.world->tick(1.0F/60.0F);
    require(crouch.world->gameplay().character(crouch.pawn)->stance == dve::CharacterStance::Crouched,
            "character stood up through a low ceiling");

    const auto secondPlayer = crouch.world->gameplay().create_player("Second", false);
    require(!crouch.world->gameplay().possess(secondPlayer, crouch.pawn, &error),
            "two players possessed the same pawn");
    require(crouch.world->gameplay().unpossess(crouch.player), "first player did not unpossess");
    require(crouch.world->gameplay().possess(secondPlayer, crouch.pawn, &error), error.c_str());
}

void test_one_shot_trigger() {
    auto fixture = make_fixture();
    dve::TriggerVolumeDesc trigger;
    trigger.name = "One Shot";
    trigger.transform.position = {0,0,1};
    trigger.halfExtents = {1,1,2};
    trigger.oneShot = true;
    std::string error;
    const auto id = fixture.world->gameplay().create_trigger(trigger, &error);
    require(id != dve::kInvalidGameTriggerId, error.c_str());
    int enters = 0;
    fixture.world->gameplay().on_trigger([&](const dve::TriggerEvent& event) {
        if (event.trigger == id && event.kind == dve::TriggerEventKind::Enter) ++enters;
    });
    fixture.world->tick(1.0F/60.0F);
    fixture.world->tick(1.0F/60.0F);
    require(enters == 1, "one-shot trigger entered more than once");
    require(fixture.world->gameplay().trigger_fired(id), "one-shot trigger did not retain fired state");
    require(!fixture.world->gameplay().trigger(id)->enabled, "one-shot trigger did not disable itself");
}

void test_moving_platform_inheritance() {
    auto physics = std::make_unique<dve::ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    auto world = std::make_unique<dve::GameWorld>(std::move(physics));
    const auto platform = add_box(*world, "Platform", 8, 8, 1, 0.25F, {-1,-1,0}, true);
    require(world->set_linear_velocity(platform, {1.0F,0,0}), "platform velocity failed");
    const auto pawn = add_marker(*world, "Pawn", {0,0,0.25F});
    std::string error;
    require(world->gameplay().add_character(pawn, {}, &error), error.c_str());
    tick(*world, 5);
    require(world->gameplay().character(pawn)->grounded, "pawn did not ground on moving platform");
    const float before = world->position(pawn)->x;
    tick(*world, 30);
    const float after = world->position(pawn)->x;
    require(after > before + 0.30F, "pawn did not inherit platform motion");
    world->gameplay().set_character_input(pawn, {{0,0,0},true,false});
    world->tick(1.0F/60.0F);
    require(world->gameplay().character(pawn)->velocity.x > 0.5F,
            "jump did not inherit platform velocity");
}
}

int main() {
    try {
        test_walk_step_wall_jump_crouch();
        test_trigger_enter_stay_exit_and_state();
        test_floor_removal_and_replay();
        test_jump_windows_crouch_clearance_and_possession();
        test_one_shot_trigger();
        test_moving_platform_inheritance();
        std::cout << "gameplay runtime tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "gameplay runtime tests failed: " << exception.what() << '\n';
        return 1;
    }
}
