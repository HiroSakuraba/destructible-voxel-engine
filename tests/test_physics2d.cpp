#include "dve/physics2d.hpp"

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

bool approx(float a, float b, float tolerance = 0.2F) {
    return std::fabs(a - b) <= tolerance;
}

TileMap make_map() {
    TileMap map;
    map.name = "physics2d_test";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "physics2d_tiles";
    map.tileset.textureAsset = "test.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 2;
    map.tileset.tiles = {
        TileDef{"solid", 0, TileCollision::Solid},
        TileDef{"one_way", 1, TileCollision::OneWayTop},
    };
    TileLayer layer;
    layer.name = "collision";
    layer.width = 10;
    layer.height = 8;
    layer.tiles.assign(80, 0U);
    for (std::uint32_t col = 0; col < layer.width; ++col) layer.set(col, 7, 1U);
    for (std::uint32_t col = 2; col <= 4; ++col) layer.set(col, 3, 2U);
    map.layers.push_back(std::move(layer));
    return map;
}

void test_native_fall_and_events() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::NativeTile;
    settings.fixedTimeStep = 1.0F / 60.0F;
    settings.maxFrameSteps = 8;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, ("native world created: " + error).c_str());
    if (!world) return;
    require(world->backend() == Physics2DBackend::NativeTile, "native backend reports itself");
    require(!world->capabilities().bodyBodyCollision, "native backend does not claim body-body collision");
    require(world->set_tile_map(make_map(), &error), ("native accepts tile map: " + error).c_str());

    Physics2DBodyDef bodyDef;
    bodyDef.positionPixels = {8.0F, 10.0F};
    bodyDef.userTag = 1001;
    const Physics2DBodyHandle body = world->create_body(bodyDef, &error);
    require(static_cast<bool>(body), ("native creates body: " + error).c_str());

    Physics2DColliderDef colliderDef;
    colliderDef.halfExtentsPixels = {6.0F, 6.0F};
    colliderDef.userTag = 2001;
    const Physics2DColliderHandle collider = world->add_collider(body, colliderDef, &error);
    require(static_cast<bool>(collider), ("native creates box collider: " + error).c_str());

    bool sawHit = false;
    for (int i = 0; i < 180; ++i) {
        world->step(settings.fixedTimeStep);
        for (const Physics2DEvent& event : world->events()) {
            if (event.type == Physics2DEventType::ContactHit && event.bodyA == body &&
                event.colliderA == collider) {
                sawHit = true;
                require(event.bodyUserTagA == 1001, "native hit event preserves body tag");
                require(event.colliderUserTagA == 2001, "native hit event preserves collider tag");
            }
        }
    }
    Physics2DBodyState state;
    require(world->body_state(body, state), "native body state is readable");
    require(approx(state.positionPixels.y, 106.0F, 0.5F), "native body lands on solid floor");
    require(approx(state.linearVelocityPixelsPerSecond.y, 0.0F), "native landing cancels vertical velocity");
    require(sawHit, "native backend emits collision hit evidence");
}

void test_native_one_way_drop_through() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::NativeTile;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, "native drop-through world created");
    if (!world) return;
    require(world->set_tile_map(make_map(), &error), "native drop-through map accepted");

    Physics2DBodyDef bodyDef;
    bodyDef.positionPixels = {48.0F, 20.0F};
    const auto body = world->create_body(bodyDef, &error);
    Physics2DColliderDef colliderDef;
    colliderDef.halfExtentsPixels = {6.0F, 6.0F};
    require(static_cast<bool>(world->add_collider(body, colliderDef, &error)), "native drop-through collider created");

    for (int i = 0; i < 90; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState state;
    require(world->body_state(body, state), "one-way body state readable");
    require(approx(state.positionPixels.y, 42.0F, 0.5F), "native body lands on one-way platform");

    require(world->drop_through_one_way(body, 0.35F), "native accepts drop-through command");
    require(world->set_body_linear_velocity(body, {0.0F, 80.0F}), "native sets drop-through velocity");
    for (int i = 0; i < 15; ++i) world->step(settings.fixedTimeStep);
    require(world->body_state(body, state), "drop-through body state remains readable");
    require(state.positionPixels.y > 52.0F, "native body passes through one-way platform");
}

void test_native_validation_and_raycast() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::NativeTile;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, "native validation world created");
    if (!world) return;
    require(world->set_tile_map(make_map(), &error), "native validation map accepted");

    Physics2DBodyDef bodyDef;
    const auto body = world->create_body(bodyDef, &error);
    Physics2DColliderDef circle;
    circle.shape = Physics2DShapeType::Circle;
    require(!world->add_collider(body, circle, &error), "native rejects unsupported circle collider");
    require(error.find("Box2D") != std::string::npos, "native rejection points to Box2D option");

    const Physics2DRayCastHit hit = world->ray_cast({8.0F, 0.0F}, {0.0F, 140.0F});
    require(hit.hit, "native tile ray cast hits floor");
    require(hit.fraction > 0.0F && hit.fraction < 1.0F, "native ray cast reports a bounded fraction");
}

TileMap make_slope_map() {
    TileMap map;
    map.name = "native_slope_map";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "native_slope_tiles";
    map.tileset.textureAsset = "slope.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 2;
    map.tileset.tiles = {
        TileDef{"solid", 0, TileCollision::Solid},
        TileDef{"slope_up_right", 1, TileCollision::SlopeUpRight},
    };
    TileLayer layer;
    layer.name = "collision";
    layer.width = 8;
    layer.height = 8;
    layer.tiles.assign(64, 0U);
    layer.set(3, 5, 2U);
    map.layers.push_back(std::move(layer));
    return map;
}

void test_native_slopes_moving_platforms_queries_and_recook() {
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::NativeTile;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world != nullptr, "native advanced world created");
    if (!world) return;
    const Physics2DCapabilities caps = world->capabilities();
    require(caps.slopeTiles && caps.kinematicPlatforms && caps.incrementalTileRecook && caps.aabbQueries,
            "native backend advertises slopes, platforms, regional recook, and AABB queries");

    TileMap map = make_slope_map();
    require(world->set_tile_map(map, &error), ("native slope map accepted: " + error).c_str());
    const Physics2DRayCastHit slopeRay = world->ray_cast({56.0F, 50.0F}, {0.0F, 60.0F});
    require(slopeRay.hit, "native ray cast hits an authored slope");
    require(slopeRay.normal.x < -0.6F && slopeRay.normal.y < -0.6F,
            "native slope ray returns a diagonal normal");

    Physics2DBodyDef characterDef;
    characterDef.positionPixels = {56.0F, 30.0F};
    const auto character = world->create_body(characterDef, &error);
    Physics2DColliderDef characterCollider;
    characterCollider.halfExtentsPixels = {6.0F, 6.0F};
    require(static_cast<bool>(world->add_collider(character, characterCollider, &error)),
            "native slope character collider created");
    for (int i = 0; i < 120; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState state;
    require(world->body_state(character, state), "native slope character state readable");
    require(state.positionPixels.y > 78.0F && state.positionPixels.y < 84.0F,
            "native dynamic body settles at the sampled ramp height");

    Physics2DBodyDef platformDef;
    platformDef.type = Physics2DBodyType::Kinematic;
    platformDef.positionPixels = {24.0F, 48.0F};
    platformDef.linearVelocityPixelsPerSecond = {30.0F, 0.0F};
    platformDef.userTag = 701;
    const auto platform = world->create_body(platformDef, &error);
    Physics2DColliderDef platformCollider;
    platformCollider.halfExtentsPixels = {80.0F, 4.0F};
    platformCollider.tangentSpeedPixelsPerSecond = 12.0F;
    platformCollider.userTag = 702;
    require(static_cast<bool>(world->add_collider(platform, platformCollider, &error)),
            "native kinematic platform collider created");

    const Physics2DRayCastHit platformRay = world->ray_cast({24.0F, 20.0F}, {0.0F, 40.0F});
    require(platformRay.hit && platformRay.body == platform,
            "native ray cast identifies a kinematic platform body");
    require(platformRay.surfaceVelocityPixelsPerSecond.x > 40.0F,
            "native platform ray combines body and conveyor surface velocity");

    Physics2DOverlapHit overlaps[2];
    const std::size_t overlapCount = world->query_aabb({{0.0F, 40.0F}, {60.0F, 20.0F}}, overlaps);
    require(overlapCount >= 1U && overlaps[0].body == platform,
            "native AABB query returns overlapping authored bodies");

    Physics2DBodyDef riderDef;
    riderDef.positionPixels = {24.0F, 10.0F};
    const auto rider = world->create_body(riderDef, &error);
    Physics2DColliderDef riderCollider;
    riderCollider.halfExtentsPixels = {5.0F, 6.0F};
    require(static_cast<bool>(world->add_collider(rider, riderCollider, &error)),
            "native moving-platform rider collider created");
    for (int i = 0; i < 90; ++i) world->step(settings.fixedTimeStep);
    Physics2DBodyState riderState;
    require(world->body_state(rider, riderState), "native moving-platform rider state readable");
    require(riderState.positionPixels.y > 37.0F && riderState.positionPixels.y < 39.0F,
            "native dynamic body lands on a moving kinematic platform");
    const Physics2DRayCastHit supportRay = world->ray_cast(
        {riderState.positionPixels.x, riderState.positionPixels.y + 5.9F}, {0.0F, 2.0F});
    require(supportRay.hit && supportRay.body == platform,
            "native ground probe identifies the moving support body after landing");

    map.layers[0].set(1, 1, 1U);
    require(!world->overlaps_tile_map({{16.0F, 16.0F}, {8.0F, 8.0F}}),
            "edited tile is absent before regional recook");
    require(world->update_tile_map_region(map, {1, 1, 1, 1}, &error),
            ("native regional recook succeeds: " + error).c_str());
    require(world->overlaps_tile_map({{16.0F, 16.0F}, {8.0F, 8.0F}}),
            "regional recook publishes the edited collision tile");
}

void test_box2d_absence_contract() {
#if defined(DVE_HAVE_BOX2D)
    require(box2d_physics_available(), "compiled Box2D build reports available");
#else
    require(!box2d_physics_available(), "default build reports Box2D unavailable");
    Physics2DWorldSettings settings;
    settings.backend = Physics2DBackend::Box2D;
    std::string error;
    auto world = create_physics2d_world(settings, &error);
    require(world == nullptr, "Box2D request fails cleanly when backend is absent");
    require(error.find("DVE_ENABLE_BOX2D") != std::string::npos, "Box2D absence error names build option");
#endif
}

} // namespace

int main() {
    test_native_fall_and_events();
    test_native_one_way_drop_through();
    test_native_validation_and_raycast();
    test_native_slopes_moving_platforms_queries_and_recook();
    test_box2d_absence_contract();
    if (failures == 0) {
        std::cout << "physics2d native: all tests passed\n";
        return 0;
    }
    std::cerr << "physics2d native: " << failures << " test(s) failed\n";
    return 1;
}
