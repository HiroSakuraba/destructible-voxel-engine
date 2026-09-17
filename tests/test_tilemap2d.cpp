#include "dve/tilemap2d.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace dve;

int g_failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

bool approx(float a, float b, float tol = 0.01F) { return std::fabs(a - b) <= tol; }

// A 10x8 test map, 16px tiles. Tile id 1 == Solid, tile id 2 == OneWayTop.
// Row 7 is a full solid floor; a solid wall sits at col 5 rows 4..6; a one-way platform spans
// cols 2..4 on row 3.
TileMap make_test_map() {
    TileMap map;
    map.name = "test_level";
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.tileset.name = "test_tiles";
    map.tileset.textureAsset = "tiles.png";
    map.tileset.tileWidth = 16;
    map.tileset.tileHeight = 16;
    map.tileset.columns = 4;
    map.tileset.tiles = {
        TileDef{"ground", 0, TileCollision::Solid},
        TileDef{"platform", 1, TileCollision::OneWayTop},
    };

    TileLayer layer;
    layer.name = "main";
    layer.width = 10;
    layer.height = 8;
    layer.tiles.assign(static_cast<std::size_t>(10) * 8, 0);
    layer.parallaxX = 1.0F;
    layer.parallaxY = 1.0F;
    layer.visible = true;
    layer.collidable = true;

    for (std::uint32_t col = 0; col < 10; ++col) layer.set(col, 7, 1); // floor
    for (std::uint32_t row = 4; row <= 6; ++row) layer.set(5, row, 1); // wall
    for (std::uint32_t col = 2; col <= 4; ++col) layer.set(col, 3, 2); // one-way platform

    map.layers = {layer};
    return map;
}

void test_serialize_roundtrip() {
    const TileMap map = make_test_map();
    std::string error;
    require(map.validate(&error), ("test map validates: " + error).c_str());

    const std::string text = map.serialize();
    TileMap parsed;
    require(TileMap::parse(text, parsed, &error), ("map parses back: " + error).c_str());
    require(parsed.serialize() == text, "serialize is a fixed point under parse");
    require(map.content_hash() == parsed.content_hash(), "content hash stable across round trip");
    require(parsed.layers.size() == 1 && parsed.layers[0].width == 10 && parsed.layers[0].height == 8,
            "layer dimensions preserved");
    require(parsed.tileset.tiles.size() == 2, "tileset preserved");
    require(parsed.tileset.tiles[1].collision == TileCollision::OneWayTop, "tile collision preserved");
}

void test_parse_rejects_malformed() {
    TileMap out;
    std::string error;
    require(!TileMap::parse("garbage", out, &error), "rejects non-dvetilemap input");
    require(!TileMap::parse("dvetilemap 1\ntile_size 0 16\n", out, &error), "rejects zero tile size");
    require(!TileMap::parse("dvetilemap 3\n", out, &error), "rejects unsupported format version");
    require(!TileMap::parse(
        "dvetilemap 1\nname bad\ntile_size 16 16\ntileset\nts_name t\n"
        "ts_texture t.png\nts_tile_size 16 16\nts_columns 1\nts_tiles 1\n"
        "tile 0 1 solid\nlayers 1\nlayer 1 1\nl_name main\n"
        "l_parallax nan 1\nl_flags 1 1\nr 1\n", out, &error),
        "rejects non-finite parallax");
    require(!TileMap::parse(
        "dvetilemap 1\nname huge\ntile_size 16 16\ntileset\nts_name t\n"
        "ts_texture t.png\nts_tile_size 16 16\nts_columns 1\nts_tiles 0\n"
        "layers 1\nlayer 4294967295 4294967295\n", out, &error),
        "rejects pathological layer dimensions before allocation");

    TileMap mismatched = make_test_map();
    mismatched.tileset.tileWidth = 8;
    require(!mismatched.validate(&error), "rejects map/tileset tile-size mismatch");
}

void test_collision_grid() {
    const TileMap map = make_test_map();
    const CollisionGrid grid = build_collision_grid(map);
    require(grid.width == 10 && grid.height == 8, "grid sized to map");
    require(grid.at(0, 7) == TileCollision::Solid, "floor cell is solid");
    require(grid.at(5, 5) == TileCollision::Solid, "wall cell is solid");
    require(grid.at(3, 3) == TileCollision::OneWayTop, "platform cell is one-way");
    require(grid.at(0, 0) == TileCollision::Empty, "empty cell is empty");
    require(grid.at(-1, 0) == TileCollision::Empty, "out-of-range reads empty");
}

void test_fall_onto_floor() {
    const CollisionGrid grid = build_collision_grid(make_test_map());
    // Mover 12x12 starting well above the floor (floor top = row 7 => y = 112).
    TileAabb box{{40.0F, 60.0F}, {12.0F, 12.0F}};
    const AabbSweep swept = move_aabb(grid, box, {0.0F, 100.0F});
    require(swept.onGround, "mover lands on the floor");
    require(swept.hitBottom, "downward hit reported");
    require(approx(swept.position.y, 112.0F - 12.0F), "mover snaps to floor surface");
    require(swept.velocity.y == 0.0F, "vertical velocity zeroed on landing");
}

void test_walk_into_wall() {
    const CollisionGrid grid = build_collision_grid(make_test_map());
    // Wall at col 5 => left face at x = 80. Mover approaches from the left on row 5.
    TileAabb box{{60.0F, 80.0F}, {12.0F, 12.0F}};
    const AabbSweep swept = move_aabb(grid, box, {30.0F, 0.0F});
    require(swept.hitRight, "mover stops at the wall");
    require(approx(swept.position.x, 80.0F - 12.0F), "mover snaps to wall face");
    require(swept.velocity.x == 0.0F, "horizontal velocity zeroed at wall");
}

void test_one_way_pass_through_from_below() {
    const CollisionGrid grid = build_collision_grid(make_test_map());
    // Platform on row 3 => top at y = 48. Mover starts below it moving up; should pass through.
    TileAabb box{{40.0F, 60.0F}, {12.0F, 12.0F}};
    const AabbSweep swept = move_aabb(grid, box, {0.0F, -30.0F});
    require(!swept.hitTop, "one-way platform does not block upward motion");
    require(approx(swept.position.y, 30.0F), "mover passes through to intended position");
}

void test_one_way_land_from_above() {
    const CollisionGrid grid = build_collision_grid(make_test_map());
    // Mover above the platform (top y = 48) moving down; should land on top.
    TileAabb box{{40.0F, 20.0F}, {12.0F, 12.0F}};
    const AabbSweep swept = move_aabb(grid, box, {0.0F, 40.0F});
    require(swept.onGround, "mover lands on the one-way platform");
    require(approx(swept.position.y, 48.0F - 12.0F), "mover snaps to platform surface");
}

void test_one_way_drop_through() {
    const CollisionGrid grid = build_collision_grid(make_test_map());
    TileAabb box{{40.0F, 20.0F}, {12.0F, 12.0F}};
    AabbMoveOptions options;
    options.ignoreOneWayPlatforms = true;
    const AabbSweep swept = move_aabb(grid, box, {0.0F, 40.0F}, options);
    require(!swept.hitBottom && !swept.onGround, "drop-through ignores one-way platform");
    require(approx(swept.position.y, 60.0F), "drop-through reaches intended position");
}

void test_ceiling_block() {
    // Build a map with a solid ceiling tile to test upward blocking.
    TileMap map = make_test_map();
    map.layers[0].set(3, 1, 1); // solid tile at col 3 row 1 => bottom face at y = 32
    const CollisionGrid grid = build_collision_grid(map);
    TileAabb box{{48.0F, 40.0F}, {12.0F, 12.0F}}; // col 3 is x 48..64
    const AabbSweep swept = move_aabb(grid, box, {0.0F, -20.0F});
    require(swept.hitTop, "mover hits the solid ceiling");
    require(approx(swept.position.y, 32.0F), "mover snaps beneath the ceiling");
}

void test_camera_dead_zone() {
    Camera2D cam;
    cam.viewWidth = 320.0F;
    cam.viewHeight = 180.0F;
    cam.deadZoneHalf = {16.0F, 12.0F};
    cam.center = {100.0F, 100.0F};
    cam.worldMax = {0.0F, 0.0F}; // clamping disabled

    camera_follow(cam, {100.0F, 100.0F});
    require(approx(cam.center.x, 100.0F) && approx(cam.center.y, 100.0F), "target at center does not move camera");

    camera_follow(cam, {130.0F, 100.0F}); // dx = 30, dead zone 16 => move 14
    require(approx(cam.center.x, 114.0F), "camera nudges to keep target at dead-zone edge");
}

void test_camera_clamp() {
    Camera2D cam;
    cam.viewWidth = 320.0F;
    cam.viewHeight = 180.0F;
    cam.deadZoneHalf = {0.0F, 0.0F};
    cam.center = {50.0F, 50.0F};
    cam.worldMin = {0.0F, 0.0F};
    cam.worldMax = {640.0F, 360.0F};

    camera_follow(cam, {0.0F, 0.0F});
    require(approx(cam.center.x, 160.0F), "camera clamps to left world edge");
    require(approx(cam.center.y, 90.0F), "camera clamps to top world edge");

    camera_follow(cam, {10000.0F, 10000.0F});
    require(approx(cam.center.x, 640.0F - 160.0F), "camera clamps to right world edge");
    require(approx(cam.center.y, 360.0F - 90.0F), "camera clamps to bottom world edge");
}

void test_parallax_offset() {
    Camera2D cam;
    cam.viewWidth = 320.0F;
    cam.viewHeight = 180.0F;
    cam.center = {300.0F, 200.0F}; // top-left = (140, 110)

    const TileVec2 foreground = parallax_layer_origin(cam, 1.0F, 1.0F);
    require(approx(foreground.x, 140.0F) && approx(foreground.y, 110.0F), "foreground tracks camera exactly");

    const TileVec2 distant = parallax_layer_origin(cam, 0.5F, 0.5F);
    require(approx(distant.x, 70.0F) && approx(distant.y, 55.0F), "distant layer lags at half speed");
}

void test_slope_tiles_and_incremental_grid_update() {
    TileMap map = make_test_map();
    map.tileset.tiles.push_back(TileDef{"slope_up_right", 2, TileCollision::SlopeUpRight});
    map.tileset.tiles.push_back(TileDef{"slope_up_left", 3, TileCollision::SlopeUpLeft});
    map.layers[0].set(1, 6, 3U);
    map.layers[0].set(2, 6, 4U);

    std::string error;
    const std::string text = map.serialize();
    TileMap parsed;
    require(TileMap::parse(text, parsed, &error), ("slope map parses: " + error).c_str());
    require(parsed.tileset.tiles[2].collision == TileCollision::SlopeUpRight,
            "slope-up-right collision survives serialization");
    require(parsed.tileset.tiles[3].collision == TileCollision::SlopeUpLeft,
            "slope-up-left collision survives serialization");

    CollisionGrid grid = build_collision_grid(map);
    TileAabb box{{18.0F, 70.0F}, {8.0F, 8.0F}};
    const AabbSweep swept = move_aabb(grid, box, {0.0F, 40.0F});
    require(swept.onGround && swept.hitBottom, "mover lands on an authored slope");
    require(swept.groundNormal.x < -0.6F && swept.groundNormal.y < -0.6F,
            "slope reports its diagonal ground normal");
    require(approx(swept.position.y, 98.0F, 0.1F), "slope height is sampled at the mover foot");

    map.layers[0].set(1, 6, 0U);
    require(update_collision_grid_region(grid, map, 1, 6, 1, 6),
            "regional collision-grid update succeeds");
    require(grid.at(1, 6) == TileCollision::Empty,
            "regional collision-grid update removes an edited slope");
    require(!update_collision_grid_region(grid, map, 9, 7, 10, 7),
            "regional update rejects an out-of-range rectangle");
}

} // namespace

int main() {
    test_serialize_roundtrip();
    test_parse_rejects_malformed();
    test_collision_grid();
    test_fall_onto_floor();
    test_walk_into_wall();
    test_one_way_pass_through_from_below();
    test_one_way_land_from_above();
    test_one_way_drop_through();
    test_ceiling_block();
    test_slope_tiles_and_incremental_grid_update();
    test_camera_dead_zone();
    test_camera_clamp();
    test_parallax_offset();

    if (g_failures == 0) {
        std::cout << "tilemap2d: all tests passed\n";
        return 0;
    }
    std::cerr << "tilemap2d: " << g_failures << " test(s) failed\n";
    return 1;
}
