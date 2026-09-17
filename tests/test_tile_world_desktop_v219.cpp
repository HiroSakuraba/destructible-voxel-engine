#include "dve/tilemap_authoring.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

const TileObject* find_object(const TileMap& map, std::size_t layer, std::uint64_t id) {
    if (layer >= map.objectLayers.size()) return nullptr;
    const auto& objects = map.objectLayers[layer].objects;
    const auto it = std::find_if(objects.begin(), objects.end(),
        [&](const TileObject& object) { return object.id == id; });
    return it == objects.end() ? nullptr : &*it;
}

void test_desktop_tile_workflow() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::filesystem::path sourceMap = root / "assets/tilemaps/v213_original_level.dvetilemap";
    const std::filesystem::path sourceTileset = root / "assets/tilemaps/v213_original_tiles.dvetileset";

    TileWorldDesktopWorkspace workspace;
    std::string error;
    require(workspace.open(sourceMap, sourceTileset, &error),
            "opens original map and external tileset: " + error);
    workspace.set_preview_camera(Camera2D{{160.0F, 90.0F}, 320.0F, 180.0F, 1.0F});

    const TileCanvasRect canvasViewport{160.0F, 40.0F, 640.0F, 480.0F};
    const TileCanvasRect paletteViewport{8.0F, 40.0F, 144.0F, 480.0F};
    TileWorldDesktopFrame frame = workspace.frame(canvasViewport, paletteViewport, 12U);
    require(!frame.canvas.cells.empty() && !frame.palette.cells.empty(),
            "builds map canvas and tile palette");
    require(!frame.layers.empty() && !frame.inspector.fields.empty(),
            "builds layer controls and full inspector");
    require(!frame.autotileRules.empty(), "visualizes autotile rules");
    require(!frame.parallaxLayers.empty(), "builds parallax preview inventory");
    require(!frame.chunks.empty(), "builds chunk diagnostics");
    require(frame.validForPlay, "original level validates for play");

    require(workspace.select_tile_layer(0U), "selects tile layer");
    TileLayer layer = workspace.canvas().session().map().layers[0U];
    layer.visible = !layer.visible;
    layer.locked = true;
    layer.parallaxX = 0.75F;
    layer.parallaxY = 0.9F;
    require(workspace.update_selected_tile_layer(layer, &error),
            "updates visibility, locking, collision, and parallax fields: " + error);
    frame = workspace.frame(canvasViewport, paletteViewport, 13U);
    require(frame.layers[0U].locked && frame.layers[0U].parallaxX == 0.75F,
            "layer frame reflects controls");

    std::size_t objectLayer = 0U;
    std::uint64_t objectId = 0U;
    for (std::size_t layerIndex = 0U;
         layerIndex < workspace.canvas().session().map().objectLayers.size(); ++layerIndex) {
        const auto& objects = workspace.canvas().session().map().objectLayers[layerIndex].objects;
        const auto candidate = std::find_if(objects.begin(), objects.end(), [](const TileObject& object) {
            return object.shape == TileObjectShape::Rectangle ||
                   object.shape == TileObjectShape::Ellipse;
        });
        if (candidate != objects.end()) {
            objectLayer = layerIndex;
            objectId = candidate->id;
            break;
        }
    }
    require(objectId != 0U && workspace.select_object(objectLayer, objectId),
            "selects a rectangle or ellipse object for collision editing");
    const TileObject* before = find_object(workspace.canvas().session().map(), objectLayer, objectId);
    require(before != nullptr, "selected object exists");
    const TileVec2 newMinimum = before == nullptr ? TileVec2{} : before->position;
    const TileVec2 newSize = before == nullptr ? TileVec2{24.0F, 24.0F}
                                               : TileVec2{before->size.x + 8.0F,
                                                          before->size.y + 4.0F};
    require(workspace.resize_selected_collision_shape(newMinimum, newSize, 5.0F, &error),
            "direct collision shape editing succeeds: " + error);
    const TileObject* resized = find_object(workspace.canvas().session().map(), objectLayer, objectId);
    require(resized != nullptr && resized->size.x == newSize.x && resized->rotationDegrees == 5.0F,
            "collision edit updates exact shape fields");

    workspace.canvas().set_tool(TileMapTool::CollisionShape);
    frame = workspace.frame(canvasViewport, paletteViewport, 13U);
    const auto selectedFrame = std::find_if(frame.canvas.objects.begin(), frame.canvas.objects.end(),
        [&](const TileCanvasObjectFrame& object) { return object.objectId == objectId && object.selected; });
    require(selectedFrame != frame.canvas.objects.end(),
            "selected collision object publishes direct resize handles");
    if (selectedFrame != frame.canvas.objects.end()) {
        const TileCanvasRect rightHandle = selectedFrame->resizeHandles[1U];
        const TileVec2 handleCenter{rightHandle.x + rightHandle.width * 0.5F,
                                   rightHandle.y + rightHandle.height * 0.5F};
        require(workspace.canvas_pointer_down(1, handleCenter, canvasViewport, &error),
                "right collision handle captures pointer: " + error);
        require(workspace.canvas_pointer_move({handleCenter.x + 16.0F, handleCenter.y},
                                              canvasViewport, &error),
                "captured collision handle tracks pointer: " + error);
        require(workspace.canvas_pointer_up(1, {handleCenter.x + 16.0F, handleCenter.y},
                                            canvasViewport, &error),
                "right collision handle commits resize: " + error);
        const TileObject* handleResized = find_object(
            workspace.canvas().session().map(), objectLayer, objectId);
        require(handleResized != nullptr && handleResized->size.x > newSize.x,
                "mouse handle directly expands collision shape");
    }

    std::uint64_t prefabId = 0U;
    require(workspace.place_prefab(objectLayer, "prefabs/sun_token.dveprefab", {120.0F, 80.0F},
                                   {{"variant", "gold"}, {"respawn", "false"}},
                                   &prefabId, &error),
            "places prefab with property overrides: " + error);
    require(prefabId != 0U && workspace.set_prefab_override("difficulty", "normal", &error),
            "edits prefab override in selected object: " + error);
    const TileObject* prefab = find_object(workspace.canvas().session().map(), objectLayer, prefabId);
    require(prefab != nullptr && prefab->properties.size() >= 3U,
            "prefab retains authored overrides");

    frame = workspace.frame(canvasViewport, paletteViewport, 14U);
    require(frame.recookOverlay.has_value() && frame.recookOverlay->renderer,
            "publishes regional renderer/physics recook overlay");
    require(frame.inspector.kind == TileWorldInspectorKind::Object ||
            frame.inspector.kind == TileWorldInspectorKind::Spawn,
            "selected prefab receives object/spawn inspector");

    require(workspace.request_play_test(&error), "one-click level validation requests play: " + error);
    require(workspace.consume_play_request(), "play request is consumed once");
    require(!workspace.consume_play_request(), "play request clears after consumption");

    const std::filesystem::path temp = std::filesystem::temp_directory_path() /
        "dve_v219_tile_world_desktop_test.dvetilemap";
    require(workspace.canvas().session().save(temp, &error),
            "edited map saves transactionally: " + error);
    std::ifstream input(temp);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    TileMap reread;
    require(TileMap::parse(buffer.str(), reread, &error),
            "saved desktop-editor map rereads: " + error);
    std::error_code ec;
    std::filesystem::remove(temp, ec);
}

} // namespace

int main() {
    test_desktop_tile_workflow();
    if (failures != 0) {
        std::cerr << failures << " tile-world desktop test(s) failed\n";
        return 1;
    }
    std::cout << "tile-world desktop v2.19 tests passed\n";
    return 0;
}
