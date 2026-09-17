#include "dve/character_controller2d.hpp"

#include <cstdlib>
#include <iostream>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace dve;

[[noreturn]] void fail_test(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}
void require(bool condition, const std::string& message) { if (!condition) fail_test(message); }

TileMap make_floor_map() {
    TileMap map;
    map.name = "controller_floor";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "controller_tiles";
    map.tileset.textureAsset = "controller.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 1;
    map.tileset.tiles.push_back(TileDef{"solid", 0, TileCollision::Solid});
    TileLayer layer;
    layer.name = "collision";
    layer.width = 8;
    layer.height = 6;
    layer.tiles.assign(48, 0U);
    for (std::uint32_t col = 0; col < layer.width; ++col) layer.set(col, 4, 1U);
    map.layers.push_back(std::move(layer));
    return map;
}

class ProbeWorld final : public Physics2DWorld {
public:
    Physics2DBackend backend() const noexcept override { return Physics2DBackend::Box2D; }
    Physics2DCapabilities capabilities() const noexcept override { return {}; }
    const Physics2DWorldSettings& settings() const noexcept override { return settings_; }
    bool set_tile_map(const TileMap&, std::string*) override { return true; }
    void clear_tile_map() override {}
    Physics2DBodyHandle create_body(const Physics2DBodyDef& def, std::string*) override {
        const Physics2DBodyHandle handle{next_++};
        Physics2DBodyState state;
        state.positionPixels = def.positionPixels;
        state.angleRadians = def.angleRadians;
        state.linearOffsetPixels = def.linearOffsetPixels;
        state.angularOffsetRadians = def.angularOffsetRadians;
        state.linearVelocityPixelsPerSecond = def.linearVelocityPixelsPerSecond;
        state.angularVelocityRadiansPerSecond = def.angularVelocityRadiansPerSecond;
        state.awake = true;
        state.enabled = def.enabled;
        states_[handle.value] = state;
        return handle;
    }
    bool destroy_body(Physics2DBodyHandle body) override { return states_.erase(body.value) != 0U; }
    Physics2DColliderHandle add_collider(Physics2DBodyHandle, const Physics2DColliderDef&, std::string*) override {
        return Physics2DColliderHandle{next_++};
    }
    bool destroy_collider(Physics2DColliderHandle) override { return true; }
    void step(float) override {}
    bool body_state(Physics2DBodyHandle body, Physics2DBodyState& out) const override {
        const auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        out = it->second;
        return true;
    }
    bool set_body_transform(Physics2DBodyHandle body, TileVec2 position, float angle) override {
        auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.positionPixels = position;
        it->second.angleRadians = angle;
        return true;
    }
    bool set_body_linear_velocity(Physics2DBodyHandle body, TileVec2 velocity) override {
        auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.linearVelocityPixelsPerSecond = velocity;
        lastVelocity = velocity;
        return true;
    }
    bool set_body_gravity_scale(Physics2DBodyHandle body, float scale) override {
        if (!states_.contains(body.value)) return false;
        lastGravityScale = scale;
        return true;
    }
    bool apply_linear_impulse(Physics2DBodyHandle body, TileVec2 impulse) override {
        auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.linearVelocityPixelsPerSecond.x += impulse.x;
        it->second.linearVelocityPixelsPerSecond.y += impulse.y;
        return true;
    }
    bool set_body_enabled(Physics2DBodyHandle body, bool enabled) override {
        auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.enabled = enabled;
        return true;
    }
    bool drop_through_one_way(Physics2DBodyHandle body, float) override {
        dropRequested = states_.contains(body.value);
        return dropRequested;
    }
    Physics2DRayCastHit ray_cast(TileVec2, TileVec2, std::uint64_t, std::uint64_t) const override {
        return probeHit;
    }
    std::span<const Physics2DEvent> events() const noexcept override { return events_; }

    Physics2DWorldSettings settings_{};
    std::unordered_map<std::uint64_t, Physics2DBodyState> states_;
    Physics2DRayCastHit probeHit{};
    TileVec2 lastVelocity{};
    float lastGravityScale{1.0F};
    bool dropRequested{false};

private:
    std::uint64_t next_{1};
    std::vector<Physics2DEvent> events_;
};

void test_native_ground_jump_buffer_and_drop() {
    Physics2DWorldSettings worldSettings;
    worldSettings.backend = Physics2DBackend::NativeTile;
    worldSettings.gravityPixelsPerSecondSquared = {0.0F, 980.0F};
    std::string error;
    auto world = create_physics2d_world(worldSettings, &error);
    require(world != nullptr && error.empty(), "world != nullptr && error.empty()");
    require(world->set_tile_map(make_floor_map(), &error), "world->set_tile_map(make_floor_map(), &error)");

    Physics2DBodyDef bodyDef;
    bodyDef.positionPixels = {48.0F, 56.0F};
    const auto body = world->create_body(bodyDef, &error);
    Physics2DColliderDef collider;
    collider.halfExtentsPixels = {7.0F, 8.0F};
    require(static_cast<bool>(world->add_collider(body, collider, &error)), "world->add_collider(body, collider, &error)");

    CharacterController2DSettings controllerSettings;
    controllerSettings.colliderHalfExtentsPixels = collider.halfExtentsPixels;
    CharacterController2D controller(controllerSettings);
    require(controller.validate(&error), "controller.validate(&error)");
    require(controller.post_step(*world, body, 1.0F / 60.0F), "controller.post_step(*world, body, 1.0F / 60.0F)");
    require(controller.state().grounded, "controller.state().grounded");

    CharacterController2DInput jump;
    jump.jumpPressed = true;
    jump.jumpHeld = true;
    require(controller.pre_step(*world, body, jump, 1.0F / 60.0F), "controller.pre_step(*world, body, jump, 1.0F / 60.0F)");
    require(controller.state().jumpedThisFrame, "controller.state().jumpedThisFrame");
    Physics2DBodyState state;
    require(world->body_state(body, state), "world->body_state(body, state)");
    require(state.linearVelocityPixelsPerSecond.y < -300.0F, "state.linearVelocityPixelsPerSecond.y < -300.0F");

    // Early release cuts the upward velocity.
    require(controller.pre_step(*world, body, {}, 1.0F / 60.0F), "controller.pre_step(*world, body, {}, 1.0F / 60.0F)");
    require(world->body_state(body, state), "world->body_state(body, state)");
    require(state.linearVelocityPixelsPerSecond.y > -250.0F, "state.linearVelocityPixelsPerSecond.y > -250.0F");

    // Buffer a jump while airborne, then land and consume it on the next pre-step.
    require(world->set_body_transform(body, {48.0F, 30.0F}, 0.0F), "world->set_body_transform(body, {48.0F, 30.0F}, 0.0F)");
    require(world->set_body_linear_velocity(body, {0.0F, 100.0F}), "world->set_body_linear_velocity(body, {0.0F, 100.0F})");
    require(controller.post_step(*world, body, 1.0F / 60.0F), "controller.post_step(*world, body, 1.0F / 60.0F)");
    CharacterController2DInput buffered;
    buffered.jumpPressed = true;
    buffered.jumpHeld = true;
    require(controller.pre_step(*world, body, buffered, 1.0F / 60.0F), "controller.pre_step(*world, body, buffered, 1.0F / 60.0F)");
    require(!controller.state().jumpedThisFrame, "!controller.state().jumpedThisFrame");
    require(world->set_body_transform(body, {48.0F, 56.0F}, 0.0F), "world->set_body_transform(body, {48.0F, 56.0F}, 0.0F)");
    require(world->set_body_linear_velocity(body, {}), "world->set_body_linear_velocity(body, {})");
    require(controller.post_step(*world, body, 1.0F / 60.0F), "controller.post_step(*world, body, 1.0F / 60.0F)");
    require(controller.state().grounded, "controller.state().grounded");
    CharacterController2DInput held;
    held.jumpHeld = true;
    require(controller.pre_step(*world, body, held, 1.0F / 60.0F), "controller.pre_step(*world, body, held, 1.0F / 60.0F)");
    require(controller.state().jumpedThisFrame, "controller.state().jumpedThisFrame");

    // Re-ground and request a one-way drop. The native world accepts the timed ignore request.
    require(world->set_body_transform(body, {48.0F, 56.0F}, 0.0F), "world->set_body_transform(body, {48.0F, 56.0F}, 0.0F)");
    require(world->set_body_linear_velocity(body, {}), "world->set_body_linear_velocity(body, {})");
    require(controller.post_step(*world, body, 1.0F / 60.0F), "controller.post_step(*world, body, 1.0F / 60.0F)");
    CharacterController2DInput drop;
    drop.dropPressed = true;
    require(controller.pre_step(*world, body, drop, 1.0F / 60.0F), "controller.pre_step(*world, body, drop, 1.0F / 60.0F)");
    require(controller.state().droppedThroughThisFrame, "controller.state().droppedThroughThisFrame");
}

void test_slope_support_and_ladder_policy() {
    ProbeWorld world;
    Physics2DBodyDef characterDef;
    characterDef.positionPixels = {40.0F, 40.0F};
    const auto character = world.create_body(characterDef, nullptr);
    Physics2DBodyDef supportDef;
    supportDef.type = Physics2DBodyType::Kinematic;
    supportDef.positionPixels = {40.0F, 56.0F};
    supportDef.linearVelocityPixelsPerSecond = {30.0F, -10.0F};
    const auto support = world.create_body(supportDef, nullptr);

    world.probeHit.hit = true;
    world.probeHit.normal = {0.5F, -0.8660254F};
    world.probeHit.fraction = 0.2F;
    world.probeHit.body = support;
    world.probeHit.surfaceVelocityPixelsPerSecond = {20.0F, 0.0F};

    CharacterController2DSettings settings;
    settings.colliderHalfExtentsPixels = {7.0F, 8.0F};
    settings.groundAccelerationPixelsPerSecondSquared = 10000.0F;
    CharacterController2D controller(settings);
    require(controller.post_step(world, character, 1.0F / 60.0F), "controller.post_step(world, character, 1.0F / 60.0F)");
    require(controller.state().grounded, "controller.state().grounded");
    require(controller.state().onSlope, "controller.state().onSlope");
    require(controller.state().supportBody == support, "controller.state().supportBody == support");
    require(std::fabs(controller.state().supportVelocityPixelsPerSecond.x - 30.0F) < 0.01F, "std::fabs(controller.state().supportVelocityPixelsPerSecond.x - 30.0F) < 0.01F");
    require(controller.state().slopeDegrees > 29.0F && controller.state().slopeDegrees < 31.0F, "controller.state().slopeDegrees > 29.0F && controller.state().slopeDegrees < 31.0F");

    CharacterController2DInput run;
    run.moveX = 1.0F;
    require(controller.pre_step(world, character, run, 1.0F / 60.0F), "controller.pre_step(world, character, run, 1.0F / 60.0F)");
    require(world.lastVelocity.x > 120.0F, "world.lastVelocity.x > 120.0F");
    require(world.lastVelocity.y > 50.0F, "velocity follows the descending-right slope tangent");

    // A support's horizontal velocity is inherited when jumping.
    CharacterController2DInput jump;
    jump.jumpPressed = true;
    jump.jumpHeld = true;
    require(controller.pre_step(world, character, jump, 1.0F / 60.0F), "controller.pre_step(world, character, jump, 1.0F / 60.0F)");
    require(controller.state().jumpedThisFrame, "controller.state().jumpedThisFrame");
    require(world.lastVelocity.x > 20.0F, "world.lastVelocity.x > 20.0F");
    require(world.lastVelocity.y < -350.0F, "world.lastVelocity.y < -350.0F");

    controller.set_ladder_contact(true, 44.0F, true);
    CharacterController2DInput climb;
    climb.moveY = -1.0F;
    require(controller.pre_step(world, character, climb, 1.0F / 60.0F), "controller.pre_step(world, character, climb, 1.0F / 60.0F)");
    require(controller.state().climbingLadder, "controller.state().climbingLadder");
    require(std::fabs(world.lastGravityScale) < 0.001F, "std::fabs(world.lastGravityScale) < 0.001F");
    require(world.lastVelocity.y < -100.0F, "world.lastVelocity.y < -100.0F");

    const CharacterController2DSnapshot snapshot = controller.capture_snapshot();
    require(controller.apply_knockback(world, character, {-180.0F, -220.0F}), "controller.apply_knockback(world, character, {-180.0F, -220.0F})");
    require(!controller.state().grounded && world.lastVelocity.x < -170.0F, "!controller.state().grounded && world.lastVelocity.x < -170.0F");
    require(controller.restore_snapshot(snapshot), "controller.restore_snapshot(snapshot)");
    require(controller.state().climbingLadder, "controller.state().climbingLadder");
}

void test_validation() {
    CharacterController2DSettings invalid;
    invalid.maximumSlopeDegrees = 90.0F;
    CharacterController2D controller(invalid);
    std::string error;
    require(!controller.validate(&error), "!controller.validate(&error)");
    require(!error.empty(), "!error.empty()");
}

} // namespace

int main() {
    test_native_ground_jump_buffer_and_drop();
    test_slope_support_and_ladder_policy();
    test_validation();
    return 0;
}
