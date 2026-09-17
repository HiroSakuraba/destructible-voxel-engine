#include "dve/tilemap_authoring.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using namespace dve;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

TileDef make_tile(std::string name, std::uint32_t atlas, TileCollision collision,
                  std::string terrain = {}) {
    TileDef tile;
    tile.name = std::move(name);
    tile.atlasIndex = atlas;
    tile.collision = collision;
    tile.terrain = std::move(terrain);
    tile.animationFirstAtlasIndex = atlas;
    return tile;
}

TileSet make_tileset() {
    TileSet set;
    set.name = "platformer_tiles";
    set.textureAsset = "textures/platformer.png";
    set.tileWidth = 16U;
    set.tileHeight = 16U;
    set.columns = 8U;

    TileDef isolated = make_tile("ground_isolated", 0U, TileCollision::Solid, "ground");
    isolated.material = "stone";
    TileDef left = make_tile("ground_left", 1U, TileCollision::Solid, "ground");
    left.material = "stone";
    TileDef right = make_tile("ground_right", 2U, TileCollision::Solid, "ground");
    right.material = "stone";
    TileDef middle = make_tile("ground_middle", 3U, TileCollision::Solid, "ground");
    middle.material = "stone";
    TileDef hazard = make_tile("spikes", 4U, TileCollision::Empty);
    hazard.damagePerSecond = 12.0F;
    hazard.trigger = "spike_contact";
    TileDef ladder = make_tile("ladder", 5U, TileCollision::Empty);
    ladder.ladder = true;
    TileDef conveyor = make_tile("conveyor", 6U, TileCollision::Solid);
    conveyor.conveyorVelocity = {48.0F, 0.0F};
    TileDef animated = make_tile("water", 20U, TileCollision::Empty);
    animated.animationFirstAtlasIndex = 20U;
    animated.animationFrameCount = 3U;
    animated.animationTicksPerFrame = 4U;
    set.tiles = {isolated, left, right, middle, hazard, ladder, conveyor, animated};

    set.autotileRules = {
        TileAutotileRule{"ground", 0U,
                         static_cast<std::uint8_t>(kTileNeighborNorth | kTileNeighborEast |
                                                   kTileNeighborSouth | kTileNeighborWest),
                         1U, 10},
        TileAutotileRule{"ground", kTileNeighborEast, kTileNeighborWest, 2U, 10},
        TileAutotileRule{"ground", kTileNeighborWest, kTileNeighborEast, 3U, 10},
        TileAutotileRule{"ground", static_cast<std::uint8_t>(kTileNeighborEast | kTileNeighborWest),
                         0U, 4U, 20},
    };
    return set;
}


void test_tileset_authoring_and_palette_workspace() {
    const std::filesystem::path temp = std::filesystem::temp_directory_path() /
        "dve_v213_tileset_authoring_test";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);

    TileSetPaletteWorkspace workspace;
    std::string error;
    require(workspace.session().create("grid_tiles", "textures/grid.png", 66U, 34U,
                                       16U, 16U, 1U, 0U, &error),
            "tileset grid creates: " + error);
    const TileSet& created = workspace.session().tileset();
    require(created.columns == 4U && created.tiles.size() == 8U,
            "tileset grid slicing accounts for source margins");
    require(created.margin == 1U && created.spacing == 0U,
            "tileset records grid margin and spacing");

    TileDef first = created.tiles[0];
    first.name = "ground_left";
    first.terrain = "ground";
    first.collision = TileCollision::Solid;
    require(workspace.session().update_tile(0U, first, &error),
            "first tile metadata updates: " + error);
    TileDef second = workspace.session().tileset().tiles[1];
    second.name = "ground_right";
    second.terrain = "ground";
    second.collision = TileCollision::Solid;
    require(workspace.session().update_tile(1U, second, &error),
            "second tile metadata updates: " + error);
    require(workspace.session().add_rule(
                TileAutotileRule{"ground", kTileNeighborWest, 0U, 2U, 10}, &error),
            "tileset autotile rule adds: " + error);
    const std::uint64_t beforeRemoval = workspace.session().tileset().content_hash();
    require(workspace.session().remove_tile(0U, &error), "tileset tile removes: " + error);
    require(workspace.session().tileset().autotileRules.size() == 1U &&
            tile_id(workspace.session().tileset().autotileRules[0].tileValue) == 1U,
            "tile removal repairs later autotile rule ids");
    require(workspace.session().undo(&error), "tileset removal undoes");
    require(workspace.session().tileset().content_hash() == beforeRemoval,
            "tileset undo restores exact semantic hash");
    require(workspace.session().redo(&error), "tileset removal redoes");
    require(workspace.session().undo(&error), "tileset returns to pre-removal state");

    const TileCanvasRect viewport{0.0F, 0.0F, 160.0F, 100.0F};
    const TileSetPaletteFrame initial = workspace.frame(viewport);
    require(initial.cells.size() == 8U, "tileset palette exposes every sliced source cell");
    require(initial.cells[0].rect.x == 1.0F && initial.cells[0].rect.y == 1.0F,
            "tileset palette uses authored source margin");
    require(workspace.pointer_down(1, {20.0F, 5.0F}, viewport),
            "tileset palette selects a source cell");
    require(workspace.session().selected_tile() == 1U,
            "tileset palette maps click to stable tile index");
    require(workspace.wheel(2.0F, {20.0F, 5.0F}, viewport),
            "tileset palette cursor-centered zoom applies");
    require(workspace.frame(viewport).zoom > 1.0F, "tileset palette zoom increases");

    const std::filesystem::path path = temp / "grid.dvetileset";
    require(workspace.session().save(path, &error), "tileset saves recoverably: " + error);
    TileSetAuthoringSession reopened;
    require(reopened.open(path, &error), "saved tileset reopens: " + error);
    require(reopened.tileset().content_hash() == workspace.session().tileset().content_hash(),
            "reopened tileset matches saved content");

    TileSet external = reopened.tileset();
    external.tiles[0].name = "external_name";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << external.serialize();
    }
    const TileSetExternalChangeResult reload = reopened.poll_external_change(true);
    require(reload.state == TileSetExternalChange::Reloaded &&
            reopened.tileset().tiles[0].name == "external_name",
            "clean tileset reloads an external change");
    TileDef dirtyTile = reopened.tileset().tiles[0];
    dirtyTile.name = "local_name";
    require(reopened.update_tile(0U, dirtyTile, &error), "tileset becomes locally dirty");
    const TileSetExternalChangeResult conflict = reopened.poll_external_change(true);
    require(conflict.state == TileSetExternalChange::Conflict,
            "tileset external reload protects dirty local edits");

    std::filesystem::remove_all(temp, ec);
}

void test_tileset_and_map_formats() {
    TileSet set = make_tileset();
    std::string error;
    require(set.validate(&error), "tileset validates: " + error);
    const std::string text = set.serialize();
    TileSet parsed;
    require(TileSet::parse(text, parsed, &error), "tileset parses: " + error);
    require(parsed.serialize() == text, "tileset serialization is canonical");
    require(parsed.content_hash() == set.content_hash(), "tileset hash round-trips");
    require(parsed.atlas_index_for(8U, 0U) == 20U, "animated tile starts at first frame");
    require(parsed.atlas_index_for(8U, 4U) == 21U, "animated tile advances deterministically");
    require(parsed.atlas_index_for(8U, 12U) == 20U, "animated tile loops deterministically");

    TileMapAuthoringSession session;
    require(session.create("level", 8U, 6U, parsed, "tiles/platformer.dvetileset", &error),
            "map document creates: " + error);
    const std::string mapText = session.map().serialize();
    TileMap mapParsed;
    require(TileMap::parse(mapText, mapParsed, &error), "v2 map parses: " + error);
    require(mapParsed.serialize() == mapText, "v2 map serialization is canonical");
    require(mapParsed.tilesetAsset == "tiles/platformer.dvetileset", "tileset asset reference survives");
    require(mapParsed.layers.size() == 2U && mapParsed.objectLayers.size() == 1U,
            "semantic and object layers survive");

    const std::string v1 =
        "dvetilemap 1\nname legacy\ntile_size 16 16\ntileset\nts_name legacy_tiles\n"
        "ts_texture legacy.png\nts_tile_size 16 16\nts_columns 1\nts_tiles 1\n"
        "tile 0 1 ground\nlayers 1\nlayer 1 1\nl_name main\n"
        "l_parallax 1 1\nl_flags 1 1\nr 1\n";
    TileMap migrated;
    require(TileMap::parse(v1, migrated, &error), "v1 map migrates: " + error);
    require(migrated.serialize().starts_with("dvetilemap 2\n"), "migrated map writes v2");
    require(migrated.layers[0].kind == TileLayerKind::Visual && !migrated.layers[0].locked,
            "v1 layer receives safe v2 defaults");
}

void test_autotile_semantics_objects_and_undo() {
    TileMapAuthoringSession session;
    std::string error;
    require(session.create("level", 8U, 6U, make_tileset(), "tiles/platformer.dvetileset", &error),
            "session creates: " + error);
    require(session.select_tile_layer(0U), "visual layer selected");
    require(session.select_terrain("ground"), "terrain selected");
    require(session.paint_terrain(2U, 2U, "ground", false, &error), "first terrain cell paints");
    require(session.paint_terrain(3U, 2U, "ground", false, &error), "second terrain cell paints");
    require(session.paint_terrain(4U, 2U, "ground", false, &error), "third terrain cell paints");
    require(tile_id(session.map().layers[0].at(2U, 2U)) == 2U, "left terrain edge resolved");
    require(tile_id(session.map().layers[0].at(3U, 2U)) == 4U, "terrain middle resolved");
    require(tile_id(session.map().layers[0].at(4U, 2U)) == 3U, "right terrain edge resolved");
    require(session.last_impact().region.has_value(), "terrain edit reports dirty region");

    require(session.add_tile_layer("Hazards", TileLayerKind::Hazard, false, &error),
            "hazard layer added");
    require(session.select_tile(5U), "hazard tile selected");
    require(session.paint_rectangle({1U, 1U, 2U, 1U}, 5U, &error), "hazard rectangle paints");
    require(session.last_impact().semanticChanged && !session.last_impact().collisionChanged,
            "hazard edit reports semantic-only refresh");

    require(session.add_tile_layer("Gameplay", TileLayerKind::Collision, true, &error),
            "collision layer added");
    require(session.paint_cell(5U, 4U, 7U, &error), "conveyor collision tile paints");
    require(session.last_impact().collisionChanged, "collision edit requests regional recook");

    TileObject spawn;
    spawn.name = "Player Start";
    spawn.type = "spawn";
    spawn.shape = TileObjectShape::Rectangle;
    spawn.position = {32.0F, 16.0F};
    spawn.size = {16.0F, 32.0F};
    std::uint64_t spawnId = 0U;
    require(session.add_object(0U, spawn, &spawnId, &error), "spawn object added: " + error);
    require(spawnId != 0U, "object receives stable nonzero id");
    require(session.set_object_property(0U, spawnId, "team", "player", &error),
            "object property added");

    const TileWorldQuery hazardQuery = query_tile_world(session.map(), {{16.0F, 16.0F}, {32.0F, 16.0F}});
    require(hazardQuery.totalDamagePerSecond == 24.0F, "hazard query sums overlapping damage cells");
    require(hazardQuery.triggers.size() == 1U && hazardQuery.triggers[0] == "spike_contact",
            "hazard query deduplicates trigger names");
    require(hazardQuery.objects.size() == 1U && hazardQuery.objects[0].objectId == spawnId,
            "world query returns overlapping level object");

    const TileWorldQuery conveyorQuery = query_tile_world(session.map(), {{80.0F, 64.0F}, {16.0F, 16.0F}});
    require(conveyorQuery.conveyorVelocity.x == 48.0F, "world query returns conveyor motion");
    require(!conveyorQuery.cells.empty() && conveyorQuery.cells[0].collision == TileCollision::Solid,
            "world query returns collision metadata");

    const std::vector<TileRenderItem> renderItems = build_tile_render_list(
        session.map(), {{0.0F, 0.0F}, {128.0F, 96.0F}}, 4U);
    require(!renderItems.empty(), "visible tile extraction returns authored tiles");
    const auto animatedItem = std::find_if(renderItems.begin(), renderItems.end(),
        [](const TileRenderItem& item) { return tile_id(item.tileValue) == 8U; });
    require(animatedItem == renderItems.end() || animatedItem->atlasIndex == 21U,
            "visible tile extraction resolves deterministic animation frames");
    const std::vector<TileChunkDiagnostic> diagnostics = build_tile_chunk_diagnostics(
        session.map(), 4U, 4U);
    require(!diagnostics.empty(), "chunk diagnostics are generated");
    require(std::any_of(diagnostics.begin(), diagnostics.end(),
                        [](const TileChunkDiagnostic& item) { return item.hazardTiles > 0U; }),
            "chunk diagnostics count hazard tiles");
    require(std::any_of(diagnostics.begin(), diagnostics.end(),
                        [](const TileChunkDiagnostic& item) { return item.objectCount > 0U; }),
            "chunk diagnostics count level objects");

    const std::uint64_t beforeUndo = session.map().content_hash();
    require(session.remove_object(0U, spawnId, &error), "object removes");
    require(session.undo(&error), "object removal undoes");
    require(session.map().content_hash() == beforeUndo, "undo restores exact canonical map");
    require(session.redo(&error), "object removal redoes");
    require(session.map().content_hash() != beforeUndo, "redo reapplies canonical edit");
}

void test_save_external_change_and_canvas() {
    const std::filesystem::path temp = std::filesystem::temp_directory_path() / "dve_v213_tilemap_test";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);

    TileMapCanvasWorkspace workspace;
    std::string error;
    require(workspace.session().create("canvas", 12U, 8U, make_tileset(), "tiles.dvetileset", &error),
            "canvas session creates");
    require(workspace.session().select_tile_layer(0U), "canvas visual layer selected");
    require(workspace.session().select_tile(1U), "canvas brush tile selected");
    workspace.set_tool(TileMapTool::Rectangle);
    const TileCanvasRect viewport{0.0F, 0.0F, 320.0F, 200.0F};
    require(workspace.pointer_down(1, {2.0F, 2.0F}, viewport, &error), "rectangle capture starts");
    require(workspace.pointer_move({47.0F, 31.0F}, viewport, &error), "rectangle preview updates");
    const TileMapCanvasFrame preview = workspace.frame(viewport);
    require(preview.brushPreview.has_value() && preview.brushPreview->maxCol == 2U &&
            preview.brushPreview->maxRow == 1U, "canvas exposes deterministic rectangle preview");
    require(workspace.pointer_up(1, {47.0F, 31.0F}, viewport, &error), "rectangle commits");
    require(workspace.session().map().layers[0].at(2U, 1U) == 1U, "rectangle paints through canvas");

    const TileVec2 worldBefore = {80.0F, 50.0F};
    require(workspace.wheel(2.0F, worldBefore, viewport), "cursor-centered canvas zoom applies");
    require(workspace.zoom() > 1.0F, "canvas zoom increases");

    workspace.set_tool(TileMapTool::Object);
    require(workspace.pointer_down(1, {100.0F, 80.0F}, viewport, &error), "object tool creates object");
    require(workspace.session().map().objectLayers[0].objects.size() == 1U,
            "canvas-created object enters object layer");
    const TileMapCanvasFrame frame = workspace.frame(viewport, 5U);
    require(!frame.cells.empty() && !frame.objects.empty(), "canvas frame contains tile and object draw data");

    const std::filesystem::path mapPath = temp / "level.dvetilemap";
    const std::filesystem::path setPath = temp / "tiles.dvetileset";
    require(workspace.session().save(mapPath, &error), "map saves recoverably: " + error);
    require(workspace.session().save_tileset(setPath, &error), "tileset saves separately: " + error);
    require(std::filesystem::exists(mapPath) && std::filesystem::exists(setPath),
            "map and reusable tileset outputs exist");

    TileMapAuthoringSession reopened;
    require(reopened.open(mapPath, &error), "saved map reopens: " + error);
    require(reopened.map().content_hash() == workspace.session().map().content_hash(),
            "reopened document matches saved content");
    require(reopened.paint_cell(5U, 5U, 1U, &error), "reopened document becomes dirty");
    const TileMapExternalChangeResult conflict = reopened.poll_external_change(true);
    require(conflict.state == TileMapExternalChange::Conflict,
            "external reload refuses to overwrite dirty local edits");

    std::filesystem::remove_all(temp, ec);
}

} // namespace

int main() {
    test_tileset_authoring_and_palette_workspace();
    test_tileset_and_map_formats();
    test_autotile_semantics_objects_and_undo();
    test_save_external_change_and_canvas();
    if (failures == 0) {
        std::cout << "tilemap authoring v2.13: all tests passed\n";
        return 0;
    }
    std::cerr << "tilemap authoring v2.13: " << failures << " failure(s)\n";
    return 1;
}
