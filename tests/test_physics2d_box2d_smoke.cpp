#include "dve/physics2d.hpp"

#include <iostream>
#include <string>

namespace {
using namespace dve;
int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    std::string error;
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    settings.gravityPixelsPerSecondSquared = {0.0F, 600.0F};
    settings.pixelsPerMeter = 32.0F;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, ("Box2D world creation: " + error).c_str());
    if (!world) return 1;

    Physics2DBodyDef dynamicDef;
    dynamicDef.positionPixels = {24.0F, 12.0F};
    dynamicDef.userTag = 101;
    const auto dynamicBody = world->create_body(dynamicDef, &error);
    require(static_cast<bool>(dynamicBody), ("dynamic body: " + error).c_str());

    Physics2DColliderDef box;
    box.halfExtentsPixels = {6.0F, 8.0F};
    box.userTag = 201;
    const auto boxCollider = world->add_collider(dynamicBody, box, &error);
    require(static_cast<bool>(boxCollider), ("box collider: " + error).c_str());

    Physics2DColliderDef sensor;
    sensor.shape = Physics2DShapeType::Circle;
    sensor.localCenterPixels = {12.0F, 0.0F};
    sensor.radiusPixels = 4.0F;
    sensor.sensor = true;
    sensor.userTag = 202;
    require(static_cast<bool>(world->add_collider(dynamicBody, sensor, &error)),
            ("sensor collider: " + error).c_str());

    Physics2DBodyState before;
    require(world->body_state(dynamicBody, before), "body state before step");
    for (int i = 0; i < 10; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState after;
    require(world->body_state(dynamicBody, after), "body state after step");
    require(after.positionPixels.y > before.positionPixels.y, "Box2D gravity advances body in DVE y-down coordinates");

    const auto hit = world->ray_cast({0.0F, after.positionPixels.y}, {80.0F, 0.0F});
    require(hit.hit, "Box2D ray cast reaches authored colliders");
    require(hit.fraction >= 0.0F && hit.fraction <= 1.0F, "ray fraction is normalized");

    require(world->set_body_linear_velocity(dynamicBody, {30.0F, -20.0F}), "velocity update");
    require(world->apply_linear_impulse(dynamicBody, {4.0F, 5.0F}), "impulse update");
    require(world->set_body_enabled(dynamicBody, false), "body disable");
    require(world->body_state(dynamicBody, after) && !after.enabled, "disabled state reflected");
    require(world->set_body_enabled(dynamicBody, true), "body enable");
    require(world->drop_through_one_way(dynamicBody, 0.2F), "drop-through command accepted");
    require(world->destroy_body(dynamicBody), "body destruction");

    if (failures == 0) {
        std::cout << "physics2d Box2D shipping path: all tests passed\n";
        return 0;
    }
    std::cerr << "physics2d Box2D shipping path: " << failures << " test(s) failed\n";
    return 1;
}
