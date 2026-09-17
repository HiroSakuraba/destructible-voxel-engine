#include "dve/physics2d.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

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
    map.name = "box2d_contract_map";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "contract_tiles";
    map.tileset.textureAsset = "contract.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 2;
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
    require(static_cast<bool>(world.add_collider(body, box, &error)), ("Box2D box collider: " + error).c_str());

    Physics2DColliderDef circle;
    circle.shape = Physics2DShapeType::Circle;
    circle.localCenterPixels = {14.0F, 0.0F};
    circle.radiusPixels = 4.0F;
    circle.userTag = 12;
    require(static_cast<bool>(world.add_collider(body, circle, &error)), ("Box2D circle collider: " + error).c_str());

    Physics2DColliderDef capsule;
    capsule.shape = Physics2DShapeType::Capsule;
    capsule.localCenterPixels = {-14.0F, 0.0F};
    capsule.capsulePoint1Pixels = {0.0F, -3.0F};
    capsule.capsulePoint2Pixels = {0.0F, 3.0F};
    capsule.radiusPixels = 3.0F;
    capsule.userTag = 13;
    require(static_cast<bool>(world.add_collider(body, capsule, &error)), ("Box2D capsule collider: " + error).c_str());

    Physics2DColliderDef polygon;
    polygon.shape = Physics2DShapeType::ConvexPolygon;
    polygon.localCenterPixels = {0.0F, -12.0F};
    polygon.verticesPixels = {{-4.0F, 3.0F}, {0.0F, -4.0F}, {4.0F, 3.0F}};
    polygon.userTag = 14;
    require(static_cast<bool>(world.add_collider(body, polygon, &error)), ("Box2D polygon collider: " + error).c_str());
}

void test_box2d_adapter_contract() {
    require(box2d_physics_available(), "contract build exposes Box2D availability");
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
    require(world->backend() == Physics2DBackend::Box2D, "Box2D backend reports itself");
    const Physics2DCapabilities caps = world->capabilities();
    require(caps.bodyBodyCollision && caps.circles && caps.capsules && caps.convexPolygons,
            "Box2D backend advertises rigid-body shape capabilities");
    require(caps.sensors && caps.rayCasts && caps.continuousCollision,
            "Box2D backend advertises sensors, queries, and CCD");
    require(caps.slopeTiles && caps.kinematicPlatforms && caps.incrementalTileRecook && caps.aabbQueries,
            "Box2D backend advertises slope cooking, platforms, regional recook, and AABB queries");
    require(world->set_tile_map(make_map(), &error), ("Box2D tile map cooking: " + error).c_str());

    Physics2DBodyDef def;
    def.positionPixels = {40.0F, 16.0F};
    def.linearVelocityPixelsPerSecond = {10.0F, 0.0F};
    def.fixedRotation = false;
    def.userTag = 9001;
    def.debugName = "contract_actor";
    const auto body = world->create_body(def, &error);
    require(static_cast<bool>(body), ("Box2D body created: " + error).c_str());
    add_all_shapes(*world, body);

    Physics2DBodyState before;
    require(world->body_state(body, before), "Box2D state readable before step");
    for (int i = 0; i < 30; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState after;
    require(world->body_state(body, after), "Box2D state readable after step");
    require(after.positionPixels.y > before.positionPixels.y, "DVE y-down gravity is converted into Box2D world units");
    require(after.positionPixels.x > before.positionPixels.x, "Box2D preserves authored pixel velocity through conversion");

    require(world->set_body_transform(body, {60.0F, 24.0F}, 0.25F), "Box2D transform setter works");
    require(world->set_body_linear_velocity(body, {}), "Box2D velocity reset works");
    require(world->set_body_gravity_scale(body, 0.0F), "Box2D gravity scale setter works");
    Physics2DBodyState gravityDisabledBefore;
    require(world->body_state(body, gravityDisabledBefore), "Box2D state readable before zero-gravity step");
    world->step(settings.fixedTimeStep);
    Physics2DBodyState gravityDisabledAfter;
    require(world->body_state(body, gravityDisabledAfter), "Box2D state readable after zero-gravity step");
    require(std::fabs(gravityDisabledAfter.positionPixels.y - gravityDisabledBefore.positionPixels.y) < 0.01F,
            "Box2D zero gravity scale suppresses gravity integration");
    require(world->set_body_gravity_scale(body, 1.0F), "Box2D gravity scale restores");
    require(world->set_body_linear_velocity(body, {20.0F, -30.0F}), "Box2D velocity setter works");
    require(world->apply_linear_impulse(body, {5.0F, 6.0F}), "Box2D impulse adapter works");
    require(world->drop_through_one_way(body, 0.25F), "Box2D drop-through timer works");
    require(world->set_body_enabled(body, false), "Box2D body disable works");
    require(world->body_state(body, after) && !after.enabled, "Box2D body reports disabled state");
    require(world->set_body_enabled(body, true), "Box2D body re-enable works");

    const Physics2DRayCastHit ray = world->ray_cast({0.0F, 0.0F}, {100.0F, 100.0F});
    require(ray.hit, "Box2D ray cast is routed through adapter");
    require(ray.fraction >= 0.0F && ray.fraction <= 1.0F, "Box2D ray cast fraction remains normalized");

    Physics2DOverlapHit overlapHits[8];
    const std::size_t overlapCount = world->query_aabb({{20.0F, 0.0F}, {80.0F, 80.0F}}, overlapHits);
    require(overlapCount >= 4U, "Box2D AABB query reports all actor colliders");
    require(world->overlaps_tile_map({{16.0F, 64.0F}, {8.0F, 8.0F}}),
            "Box2D tile-overlap query uses the cooked collision grid");

    TileMap edited = make_map();
    edited.layers[0].set(0, 0, 1U);
    require(world->update_tile_map_region(edited, {0, 0, 0, 0}, &error),
            ("Box2D regional tile recook succeeds: " + error).c_str());
    require(world->overlaps_tile_map({{0.0F, 0.0F}, {8.0F, 8.0F}}),
            "Box2D regional recook publishes the edited tile");

    require(world->events().empty(), "contract shim returns stable empty event span when no contacts are generated");
    require(world->destroy_body(body), "Box2D body destruction cleans owned colliders");
    require(!world->body_state(body, after), "destroyed Box2D handle is rejected");
}

void test_box2d_validation() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, "Box2D validation world created");
    if (!world) return;
    const auto body = world->create_body({}, &error);

    Physics2DColliderDef invalidPolygon;
    invalidPolygon.shape = Physics2DShapeType::ConvexPolygon;
    invalidPolygon.verticesPixels = {{0.0F, 0.0F}, {1.0F, 0.0F}};
    require(!world->add_collider(body, invalidPolygon, &error), "Box2D rejects polygons with fewer than three vertices");

    Physics2DColliderDef invalidCircle;
    invalidCircle.shape = Physics2DShapeType::Circle;
    invalidCircle.radiusPixels = 0.0F;
    require(!world->add_collider(body, invalidCircle, &error), "Box2D rejects zero-radius circles");
}

} // namespace

int main() {
    test_box2d_adapter_contract();
    test_box2d_validation();
    if (failures == 0) {
        std::cout << "physics2d Box2D adapter contract: all tests passed\n";
        return 0;
    }
    std::cerr << "physics2d Box2D adapter contract: " << failures << " test(s) failed\n";
    return 1;
}
