#include "dve/physics2d.hpp"

#include <algorithm>
#include <cmath>
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

TileMap make_map() {
    TileMap map;
    map.name = "box2d_upstream_map";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "upstream_tiles";
    map.tileset.textureAsset = "upstream.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 3;
    map.tileset.tiles = {
        TileDef{"solid", 0, TileCollision::Solid},
        TileDef{"one_way", 1, TileCollision::OneWayTop},
        TileDef{"slope_up_right", 2, TileCollision::SlopeUpRight},
    };
    TileLayer layer;
    layer.name = "collision";
    layer.width = 8;
    layer.height = 6;
    layer.tiles.assign(48, 0U);
    for (std::uint32_t col = 0; col < layer.width; ++col) layer.set(col, 5, 1U);
    for (std::uint32_t col = 2; col < 6; ++col) layer.set(col, 3, 2U);
    layer.set(1, 4, 3U);
    map.layers.push_back(std::move(layer));
    return map;
}

void add_all_shapes(Physics2DWorld& world, Physics2DBodyHandle body) {
    std::string error;
    Physics2DColliderDef box;
    box.shape = Physics2DShapeType::Box;
    box.halfExtentsPixels = {5.0F, 8.0F};
    box.userTag = 11;
    require(static_cast<bool>(world.add_collider(body, box, &error)),
            ("Box2D box collider: " + error).c_str());

    Physics2DColliderDef circle;
    circle.shape = Physics2DShapeType::Circle;
    circle.localCenterPixels = {14.0F, 0.0F};
    circle.radiusPixels = 4.0F;
    circle.userTag = 12;
    require(static_cast<bool>(world.add_collider(body, circle, &error)),
            ("Box2D circle collider: " + error).c_str());

    Physics2DColliderDef capsule;
    capsule.shape = Physics2DShapeType::Capsule;
    capsule.localCenterPixels = {-14.0F, 0.0F};
    capsule.capsulePoint1Pixels = {0.0F, -3.0F};
    capsule.capsulePoint2Pixels = {0.0F, 3.0F};
    capsule.radiusPixels = 3.0F;
    capsule.userTag = 13;
    require(static_cast<bool>(world.add_collider(body, capsule, &error)),
            ("Box2D capsule collider: " + error).c_str());

    Physics2DColliderDef polygon;
    polygon.shape = Physics2DShapeType::ConvexPolygon;
    polygon.localCenterPixels = {0.0F, -12.0F};
    polygon.verticesPixels = {{-4.0F, 3.0F}, {0.0F, -4.0F}, {4.0F, 3.0F}};
    polygon.userTag = 14;
    require(static_cast<bool>(world.add_collider(body, polygon, &error)),
            ("Box2D polygon collider: " + error).c_str());
}

void test_upstream_adapter() {
    require(box2d_physics_available(), "shipping build exposes Box2D availability");
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    settings.gravityPixelsPerSecondSquared = {0.0F, 960.0F};
    settings.pixelsPerMeter = 32.0F;
    settings.fixedTimeStep = 1.0F / 60.0F;
    settings.subStepCount = 4;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, ("Box2D world created: " + error).c_str());
    if (!world) return;
    require(world->set_tile_map(make_map(), &error), ("Box2D tile map cooking: " + error).c_str());

    Physics2DBodyDef def;
    def.positionPixels = {40.0F, 16.0F};
    def.linearVelocityPixelsPerSecond = {10.0F, 0.0F};
    def.fixedRotation = false;
    def.userTag = 9001;
    def.debugName = "upstream_actor";
    const auto body = world->create_body(def, &error);
    require(static_cast<bool>(body), ("Box2D body created: " + error).c_str());
    add_all_shapes(*world, body);

    Physics2DBodyState before;
    require(world->body_state(body, before), "Box2D state readable before step");
    for (int i = 0; i < 30; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState after;
    require(world->body_state(body, after), "Box2D state readable after step");
    require(after.positionPixels.y > before.positionPixels.y,
            "DVE y-down gravity converts into Box2D world units");
    require(after.positionPixels.x > before.positionPixels.x,
            "Box2D preserves authored pixel velocity through conversion");

    const Physics2DRayCastHit ray = world->ray_cast({0.0F, after.positionPixels.y}, {100.0F, 0.0F});
    require(ray.hit, "upstream Box2D ray cast reaches current body position");
    require(ray.fraction >= 0.0F && ray.fraction <= 1.0F,
            "upstream Box2D ray fraction remains normalized");

    Physics2DOverlapHit overlapHits[8];
    const std::size_t overlapCount = world->query_aabb({{20.0F, 0.0F}, {80.0F, 100.0F}}, overlapHits);
    require(overlapCount >= 4U, "upstream Box2D AABB query reports actor colliders");
    require(world->overlaps_tile_map({{16.0F, 64.0F}, {8.0F, 8.0F}}),
            "tile-overlap query uses the cooked collision grid");

    TileMap edited = make_map();
    edited.layers[0].set(0, 0, 1U);
    require(world->update_tile_map_region(edited, {0, 0, 0, 0}, &error),
            ("upstream regional tile recook succeeds: " + error).c_str());
    require(world->overlaps_tile_map({{0.0F, 0.0F}, {8.0F, 8.0F}}),
            "regional recook publishes the edited tile");

    require(world->set_body_transform(body, {60.0F, 24.0F}, 0.25F), "transform setter works");
    require(world->set_body_linear_velocity(body, {}), "velocity reset works");
    require(world->set_body_gravity_scale(body, 0.0F), "gravity scale setter works");
    Physics2DBodyState gravityDisabledBefore;
    require(world->body_state(body, gravityDisabledBefore), "state readable before zero-gravity step");
    world->step(settings.fixedTimeStep);
    Physics2DBodyState gravityDisabledAfter;
    require(world->body_state(body, gravityDisabledAfter), "state readable after zero-gravity step");
    require(std::fabs(gravityDisabledAfter.positionPixels.y - gravityDisabledBefore.positionPixels.y) < 0.02F,
            "zero gravity scale suppresses gravity integration");
    require(world->set_body_gravity_scale(body, 1.0F), "gravity scale restores");
    require(world->apply_linear_impulse(body, {5.0F, 6.0F}), "linear impulse adapter works");
    require(world->set_body_enabled(body, false), "body disable works");
    require(world->body_state(body, after) && !after.enabled, "body reports disabled state");
    require(world->set_body_enabled(body, true), "body re-enable works");
    require(world->destroy_body(body), "body destruction cleans owned colliders");
    require(!world->body_state(body, after), "destroyed body handle is rejected");
}

void test_motion_lock_and_sensor_events() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    settings.gravityPixelsPerSecondSquared = {};
    settings.pixelsPerMeter = 32.0F;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, ("motion-lock world: " + error).c_str());
    if (!world) return;

    Physics2DBodyDef lockedDef;
    lockedDef.positionPixels = {20.0F, 20.0F};
    lockedDef.angularVelocityRadiansPerSecond = 4.0F;
    lockedDef.fixedRotation = true;
    const auto lockedBody = world->create_body(lockedDef, &error);
    Physics2DColliderDef lockedBox;
    lockedBox.halfExtentsPixels = {4.0F, 4.0F};
    require(static_cast<bool>(world->add_collider(lockedBody, lockedBox, &error)),
            ("motion-lock collider: " + error).c_str());
    for (int i = 0; i < 20; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState lockedState;
    require(world->body_state(lockedBody, lockedState), "motion-lock body state");
    require(std::fabs(lockedState.angleRadians) < 0.001F,
            "Box2D 3.2 angular motion lock preserves fixed rotation");

    Physics2DBodyDef sensorBodyDef;
    sensorBodyDef.type = Physics2DBodyType::Static;
    sensorBodyDef.positionPixels = {80.0F, 20.0F};
    sensorBodyDef.userTag = 700;
    const auto sensorBody = world->create_body(sensorBodyDef, &error);
    Physics2DColliderDef sensor;
    sensor.halfExtentsPixels = {8.0F, 8.0F};
    sensor.sensor = true;
    sensor.userTag = 701;
    require(static_cast<bool>(world->add_collider(sensorBody, sensor, &error)),
            ("sensor collider: " + error).c_str());

    Physics2DBodyDef visitorDef;
    visitorDef.positionPixels = {80.0F, 20.0F};
    visitorDef.userTag = 800;
    const auto visitorBody = world->create_body(visitorDef, &error);
    Physics2DColliderDef visitor;
    visitor.halfExtentsPixels = {4.0F, 4.0F};
    visitor.userTag = 801;
    require(static_cast<bool>(world->add_collider(visitorBody, visitor, &error)),
            ("sensor visitor collider: " + error).c_str());
    world->step(settings.fixedTimeStep);
    const bool sawSensorBegin = std::ranges::any_of(world->events(), [](const Physics2DEvent& event) {
        return event.type == Physics2DEventType::SensorBegin &&
               ((event.bodyUserTagA == 700 && event.bodyUserTagB == 800) ||
                (event.bodyUserTagA == 800 && event.bodyUserTagB == 700));
    });
    require(sawSensorBegin, "upstream Box2D sensor begin event preserves DVE user tags");
}

TileMap make_one_way_map() {
    TileMap map;
    map.name = "one_way_upstream";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "one_way_tiles";
    map.tileset.textureAsset = "one_way.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 1;
    map.tileset.tiles = {TileDef{"one_way", 0, TileCollision::OneWayTop}};
    TileLayer layer;
    layer.name = "collision";
    layer.width = 8;
    layer.height = 8;
    layer.tiles.assign(64, 0U);
    for (std::uint32_t col = 0; col < layer.width; ++col) layer.set(col, 4, 1U);
    map.layers.push_back(std::move(layer));
    return map;
}

void test_one_way_platform() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    settings.gravityPixelsPerSecondSquared = {0.0F, 600.0F};
    settings.pixelsPerMeter = 32.0F;
    settings.fixedTimeStep = 1.0F / 60.0F;
    settings.subStepCount = 4;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, ("one-way world: " + error).c_str());
    if (!world) return;
    require(world->set_tile_map(make_one_way_map(), &error), ("one-way map: " + error).c_str());

    Physics2DBodyDef bodyDef;
    bodyDef.positionPixels = {40.0F, 30.0F};
    const auto body = world->create_body(bodyDef, &error);
    Physics2DColliderDef box;
    box.halfExtentsPixels = {5.0F, 5.0F};
    require(static_cast<bool>(world->add_collider(body, box, &error)),
            ("one-way actor collider: " + error).c_str());
    for (int i = 0; i < 120; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState landed;
    require(world->body_state(body, landed), "one-way landed body state");
    require(landed.positionPixels.y > 57.0F && landed.positionPixels.y < 61.0F,
            "actor lands on the authored one-way platform from above");
    require(std::fabs(landed.linearVelocityPixelsPerSecond.y) < 1.0F,
            "one-way platform resolves downward velocity");

    require(world->drop_through_one_way(body, 0.35F), "one-way drop-through starts");
    require(world->set_body_linear_velocity(body, {0.0F, 40.0F}), "one-way drop-through velocity set");
    for (int i = 0; i < 24; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState dropped;
    require(world->body_state(body, dropped), "one-way dropped body state");
    require(dropped.positionPixels.y > 70.0F,
            "drop-through timer disables the one-way contact on upstream Box2D");
}

void test_validation() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, "validation world created");
    if (!world) return;
    const auto body = world->create_body({}, &error);

    Physics2DColliderDef invalidPolygon;
    invalidPolygon.shape = Physics2DShapeType::ConvexPolygon;
    invalidPolygon.verticesPixels = {{0.0F, 0.0F}, {1.0F, 0.0F}};
    require(!world->add_collider(body, invalidPolygon, &error),
            "Box2D rejects polygons with fewer than three vertices");

    Physics2DColliderDef invalidCircle;
    invalidCircle.shape = Physics2DShapeType::Circle;
    invalidCircle.radiusPixels = 0.0F;
    require(!world->add_collider(body, invalidCircle, &error),
            "Box2D rejects zero-radius circles");
}

} // namespace

int main() {
    test_upstream_adapter();
    test_motion_lock_and_sensor_events();
    test_one_way_platform();
    test_validation();
    if (failures == 0) {
        std::cout << "physics2d upstream Box2D integration: all tests passed\n";
        return 0;
    }
    std::cerr << "physics2d upstream Box2D integration: " << failures << " test(s) failed\n";
    return 1;
}
