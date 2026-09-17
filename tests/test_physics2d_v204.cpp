#include "dve/character_controller2d.hpp"
#include "dve/component.hpp"
#include "dve/physics2d.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
[[noreturn]] void fail_test(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}
void require(bool condition, const std::string& message) { if (!condition) fail_test(message); }

using namespace dve;

Physics2DBodyHandle add_box(Physics2DWorld& world, Physics2DBodyType type,
                            TileVec2 position, TileVec2 half, bool oneWay = false) {
    std::string error;
    Physics2DBodyDef bodyDef;
    bodyDef.type = type;
    bodyDef.positionPixels = position;
    bodyDef.userTag = static_cast<std::uint64_t>(1000.0F + position.x);
    const auto body = world.create_body(bodyDef, &error);
    require(body && error.empty(), "body && error.empty()");
    Physics2DColliderDef collider;
    collider.halfExtentsPixels = half;
    collider.oneWayPlatform = oneWay;
    collider.userTag = 77U;
    require(static_cast<bool>(world.add_collider(body, collider, &error)), "world.add_collider(body, collider, &error)");
    return body;
}



class DirectionalProbeWorld final : public Physics2DWorld {
public:
    Physics2DBodyHandle character{1U};
    Physics2DBodyHandle support{2U};
    Physics2DBodyHandle leftWall{3U};
    Physics2DBodyHandle rightWall{4U};
    Physics2DBodyHandle ceiling{5U};

    DirectionalProbeWorld() {
        states_[character.value] = Physics2DBodyState{{100.0F, 100.0F}, 0.0F, {}, 0.0F, {}, 0.0F, true, true};
        states_[support.value] = Physics2DBodyState{{100.0F, 118.0F}, 0.0F, {}, 0.0F, {10.0F, 0.0F}, 0.0F, true, true};
        states_[leftWall.value] = Physics2DBodyState{{90.0F, 100.0F}, 0.0F, {}, 0.0F, {30.0F, 0.0F}, 0.0F, true, true};
        states_[rightWall.value] = Physics2DBodyState{{110.0F, 100.0F}, 0.0F, {}, 0.0F, {-30.0F, 0.0F}, 0.0F, true, true};
        states_[ceiling.value] = Physics2DBodyState{{100.0F, 84.0F}, 0.0F, {}, 0.0F, {0.0F, 40.0F}, 0.0F, true, true};
    }

    [[nodiscard]] Physics2DBackend backend() const noexcept override { return Physics2DBackend::NativeTile; }
    [[nodiscard]] Physics2DCapabilities capabilities() const noexcept override {
        Physics2DCapabilities caps;
        caps.rayCasts = true;
        caps.oneWayPlatforms = true;
        caps.kinematicPlatforms = true;
        return caps;
    }
    [[nodiscard]] const Physics2DWorldSettings& settings() const noexcept override { return settings_; }
    bool set_tile_map(const TileMap&, std::string* = nullptr) override { return true; }
    void clear_tile_map() override {}
    [[nodiscard]] Physics2DBodyHandle create_body(const Physics2DBodyDef&, std::string* = nullptr) override { return {}; }
    bool destroy_body(Physics2DBodyHandle) override { return false; }
    [[nodiscard]] Physics2DColliderHandle add_collider(Physics2DBodyHandle, const Physics2DColliderDef&, std::string* = nullptr) override { return {}; }
    bool destroy_collider(Physics2DColliderHandle) override { return false; }
    void step(float) override {}
    [[nodiscard]] bool body_state(Physics2DBodyHandle body, Physics2DBodyState& out) const override {
        const auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        out = it->second;
        return true;
    }
    bool set_body_transform(Physics2DBodyHandle body, TileVec2 positionPixels, float angleRadians) override {
        const auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.positionPixels = positionPixels;
        it->second.angleRadians = angleRadians;
        return true;
    }
    bool set_body_linear_velocity(Physics2DBodyHandle body, TileVec2 velocityPixelsPerSecond) override {
        const auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.linearVelocityPixelsPerSecond = velocityPixelsPerSecond;
        return true;
    }
    bool set_body_gravity_scale(Physics2DBodyHandle, float) override { return true; }
    bool apply_linear_impulse(Physics2DBodyHandle body, TileVec2 impulse) override {
        const auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.linearVelocityPixelsPerSecond.x += impulse.x;
        it->second.linearVelocityPixelsPerSecond.y += impulse.y;
        return true;
    }
    bool set_body_enabled(Physics2DBodyHandle body, bool enabled) override {
        const auto it = states_.find(body.value);
        if (it == states_.end()) return false;
        it->second.enabled = enabled;
        return true;
    }
    bool drop_through_one_way(Physics2DBodyHandle, float) override { return true; }

    [[nodiscard]] Physics2DRayCastHit ray_cast(TileVec2 origin, TileVec2 translation,
                                                std::uint64_t = 1,
                                                std::uint64_t = ~std::uint64_t{0}) const override {
        Physics2DRayCastHit hit;
        hit.fraction = 0.5F;
        if (translation.x < -0.01F) {
            hit.hit = true;
            hit.body = leftWall;
            hit.normal = {1.0F, 0.0F};
        } else if (translation.x > 0.01F) {
            hit.hit = true;
            hit.body = rightWall;
            hit.normal = {-1.0F, 0.0F};
        } else if (translation.y < -0.01F) {
            hit.hit = true;
            hit.body = ceiling;
            hit.normal = {0.0F, 1.0F};
        } else if (translation.y > 0.01F) {
            // Foot probes originate inside the character footprint. The ledge probe is placed
            // forward of the right edge and intentionally misses.
            if (origin.x <= 108.0F) {
                hit.hit = true;
                hit.body = support;
                hit.normal = {0.0F, -1.0F};
                hit.surfaceVelocityPixelsPerSecond = {5.0F, 0.0F};
            }
        }
        if (hit.hit) hit.pointPixels = {origin.x + translation.x * hit.fraction,
                                        origin.y + translation.y * hit.fraction};
        return hit;
    }
    [[nodiscard]] std::span<const Physics2DEvent> events() const noexcept override { return events_; }

private:
    Physics2DWorldSettings settings_{};
    std::unordered_map<std::uint64_t, Physics2DBodyState> states_;
    std::vector<Physics2DEvent> events_;
};

void test_platformer_surroundings_crush_and_transport() {
    DirectionalProbeWorld world;
    CharacterController2DSettings settings;
    settings.colliderHalfExtentsPixels = {7.0F, 14.0F};
    settings.wallProbeDistancePixels = 3.0F;
    settings.ceilingProbeDistancePixels = 3.0F;
    settings.ledgeProbeForwardPixels = 6.0F;
    settings.ledgeProbeDownPixels = 12.0F;
    settings.crushMinimumClosingSpeedPixelsPerSecond = 20.0F;
    settings.crushGraceSeconds = 0.08F;
    settings.groundAccelerationPixelsPerSecondSquared = 10000.0F;
    settings.groundDecelerationPixelsPerSecondSquared = 10000.0F;
    CharacterController2D controller(settings);

    require(controller.post_step(world, world.character, 0.05F), "first controller post_step");
    const auto& first = controller.state();
    require(first.grounded, "controller grounded on support");
    require(first.touchingLeftWall && first.touchingRightWall && first.touchingCeiling,
            "controller reports both walls and ceiling");
    require(first.nearLedge, "controller reports forward ledge");
    require(first.supportBody == world.support && first.leftWallBody == world.leftWall &&
            first.rightWallBody == world.rightWall && first.ceilingBody == world.ceiling,
            "controller preserves surrounding body handles");
    require(std::fabs(first.crushClosingSpeedPixelsPerSecond - 60.0F) < 0.01F,
            "horizontal closing speed is measured");
    require(!first.crushed, "crush grace suppresses first frame");

    require(controller.post_step(world, world.character, 0.05F), "second controller post_step");
    require(controller.state().crushed, "crush is reported after grace period");

    CharacterController2DInput idle;
    require(controller.pre_step(world, world.character, idle, 1.0F / 60.0F), "first transport pre_step");
    Physics2DBodyState bodyState;
    require(world.body_state(world.character, bodyState), "read first transported velocity");
    require(std::fabs(bodyState.linearVelocityPixelsPerSecond.x - 15.0F) < 0.01F,
            "support plus conveyor velocity applied once");
    require(controller.pre_step(world, world.character, idle, 1.0F / 60.0F), "second transport pre_step");
    require(world.body_state(world.character, bodyState), "read second transported velocity");
    require(std::fabs(bodyState.linearVelocityPixelsPerSecond.x - 15.0F) < 0.01F,
            "support and conveyor velocity do not accumulate");

    const auto snapshot = controller.capture_snapshot();
    controller.reset();
    require(controller.restore_snapshot(snapshot), "restore extended controller snapshot");
    require(controller.state().crushed && controller.state().nearLedge,
            "snapshot retains crush and ledge state");
}


void test_component_schemas() {
    const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    const ComponentTypeSchema* joint = registry.find("dve.physics2d_joint");
    require(joint != nullptr && joint->allowMultiple, "2D joint component schema is registered");
    const ComponentTypeSchema* collider = registry.find("dve.physics2d_collider");
    require(collider != nullptr, "2D collider component schema is registered");
    const auto has_property = [](const ComponentTypeSchema& schema, std::string_view name) {
        return std::any_of(schema.properties.begin(), schema.properties.end(),
                           [name](const ComponentPropertySchema& property) {
                               return property.name == name;
                           });
    };
    require(has_property(*joint, "break_force") && has_property(*joint, "spring_hertz"),
            "joint schema exposes break and spring settings");
    require(has_property(*collider, "one_way_platform") && has_property(*collider, "tangent_speed"),
            "collider schema exposes one-way and conveyor settings");
    const ComponentTypeSchema* character = registry.find("dve.sideview_character");
    require(character != nullptr && has_property(*character, "enable_crush_detection"),
            "character schema exposes crush detection");
}

void test_native_queries_and_one_way_platform() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::NativeTile;
    settings.gravityPixelsPerSecondSquared = {};
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world && world->capabilities().shapeOverlapQueries && world->capabilities().shapeCasts, "world && world->capabilities().shapeOverlapQueries && world->capabilities().shapeCasts");
    const auto solid = add_box(*world, Physics2DBodyType::Static, {50.0F, 50.0F}, {8.0F, 8.0F});
    static_cast<void>(solid);

    Physics2DQueryShape circle;
    circle.shape = Physics2DShapeType::Circle;
    circle.centerPixels = {43.0F, 50.0F};
    circle.radiusPixels = 3.0F;
    std::array<Physics2DOverlapHit, 4> hits{};
    require(world->query_shape(circle, hits) == 1U, "world->query_shape(circle, hits) == 1U");
    Physics2DQueryFilter ignored;
    ignored.ignoredBody = solid;
    require(world->query_shape(circle, hits, ignored) == 0U, "world->query_shape(circle, hits, ignored) == 0U");

    Physics2DQueryShape capsule;
    capsule.shape = Physics2DShapeType::Capsule;
    capsule.centerPixels = {20.0F, 50.0F};
    capsule.capsulePoint1Pixels = {0.0F, -3.0F};
    capsule.capsulePoint2Pixels = {0.0F, 3.0F};
    capsule.radiusPixels = 2.0F;
    const Physics2DShapeCastHit cast = world->cast_shape(capsule, {40.0F, 0.0F});
    require(cast.hit && cast.body == solid && cast.fraction > 0.0F && cast.fraction < 1.0F, "cast.hit && cast.body == solid && cast.fraction > 0.0F && cast.fraction < 1.0F");

    Physics2DQueryShape polygon;
    polygon.shape = Physics2DShapeType::ConvexPolygon;
    polygon.centerPixels = {50.0F, 50.0F};
    polygon.verticesPixels = {{-2.0F, -2.0F}, {2.0F, -2.0F}, {0.0F, 3.0F}};
    require(world->query_shape(polygon, hits) == 1U, "world->query_shape(polygon, hits) == 1U");

    Physics2DJointDef joint;
    joint.bodyA = solid;
    joint.bodyB = add_box(*world, Physics2DBodyType::Dynamic, {80.0F, 50.0F}, {4.0F, 4.0F});
    require(!world->create_joint(joint, &error), "!world->create_joint(joint, &error)");
    require(error.find("support") != std::string::npos || error.find("joint") != std::string::npos, "error.find('support') != std::string::npos || error.find('joint') != std::string::npos");

    const auto platform = add_box(*world, Physics2DBodyType::Kinematic, {100.0F, 50.0F}, {20.0F, 3.0F}, true);
    static_cast<void>(platform);
    require(!world->ray_cast({100.0F, 60.0F}, {0.0F, -20.0F}).hit, "!world->ray_cast({100.0F, 60.0F}, {0.0F, -20.0F}).hit");
    const auto down = world->ray_cast({100.0F, 35.0F}, {0.0F, 30.0F});
    require(down.hit && down.normal.y < -0.5F, "down.hit && down.normal.y < -0.5F");
}

void test_box2d_queries_and_joints_when_available() {
    if (!box2d_physics_available()) return;
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    settings.gravityPixelsPerSecondSquared = {};
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world && world->capabilities().joints, "world && world->capabilities().joints");
    const auto ground = add_box(*world, Physics2DBodyType::Static, {50.0F, 50.0F}, {10.0F, 10.0F});
    const auto actor = add_box(*world, Physics2DBodyType::Dynamic, {80.0F, 50.0F}, {5.0F, 5.0F});

    Physics2DQueryShape circle;
    circle.shape = Physics2DShapeType::Circle;
    circle.centerPixels = {50.0F, 50.0F};
    circle.radiusPixels = 4.0F;
    std::array<Physics2DOverlapHit, 8> hits{};
    require(world->query_shape(circle, hits) >= 1U, "world->query_shape(circle, hits) >= 1U");
    circle.centerPixels = {10.0F, 50.0F};
    require(world->cast_shape(circle, {50.0F, 0.0F}).hit, "world->cast_shape(circle, {50.0F, 0.0F}).hit");

    for (const Physics2DJointType type : {Physics2DJointType::Revolute, Physics2DJointType::Prismatic,
             Physics2DJointType::Distance, Physics2DJointType::Weld,
             Physics2DJointType::Wheel, Physics2DJointType::Motor}) {
        Physics2DJointDef def;
        def.type = type;
        def.bodyA = ground;
        def.bodyB = actor;
        def.lengthPixels = 30.0F;
        def.minLengthPixels = 10.0F;
        def.maxLengthPixels = 40.0F;
        def.springHertz = 3.0F;
        def.enableSpring = true;
        def.maxMotorForce = 100.0F;
        def.maxMotorTorque = 100.0F;
        def.maxVelocityForce = 100.0F;
        def.maxVelocityTorque = 100.0F;
        const auto handle = world->create_joint(def, &error);
        require(handle && error.empty(), "handle && error.empty()");
        Physics2DJointState state;
        require(world->joint_state(handle, state) && state.type == type && state.enabled, "world->joint_state(handle, state) && state.type == type && state.enabled");
        require(world->destroy_joint(handle), "world->destroy_joint(handle)");
    }
}
} // namespace

int main() {
    test_native_queries_and_one_way_platform();
    test_box2d_queries_and_joints_when_available();
    test_platformer_surroundings_crush_and_transport();
    test_component_schemas();
    return 0;
}
