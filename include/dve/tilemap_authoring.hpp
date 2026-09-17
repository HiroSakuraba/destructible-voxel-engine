#pragma once

#include "dve/tilemap2d.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dve {

struct TileCanvasRect {
    float x{};
    float y{};
    float width{};
    float height{};
    [[nodiscard]] bool contains(TileVec2 point) const noexcept {
        return point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
    }
};

enum class TileSetExternalChange : std::uint8_t {
    Unchanged,
    Reloaded,
    Conflict,
    Missing,
    Failed,
};

struct TileSetExternalChangeResult {
    TileSetExternalChange state{TileSetExternalChange::Unchanged};
    std::string message;
};

// Transactional reusable tileset document. Grid slicing is deterministic and records texture
// dimensions, while tile semantics and autotile rules remain editable without requiring a map.
class TileSetAuthoringSession {
public:
    [[nodiscard]] const TileSet& tileset() const noexcept { return tileset_; }
    [[nodiscard]] std::size_t selected_tile() const noexcept { return selectedTile_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] bool create(std::string name, std::string textureAsset,
                              std::uint32_t textureWidth, std::uint32_t textureHeight,
                              std::uint32_t tileWidth, std::uint32_t tileHeight,
                              std::uint32_t margin = 0U, std::uint32_t spacing = 0U,
                              std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path = {}, std::string* error = nullptr);
    [[nodiscard]] bool select_tile(std::size_t index) noexcept;
    [[nodiscard]] bool update_tile(std::size_t index, TileDef tile,
                                   std::string* error = nullptr);
    [[nodiscard]] bool remove_tile(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool add_rule(TileAutotileRule rule, std::string* error = nullptr);
    [[nodiscard]] bool update_rule(std::size_t index, TileAutotileRule rule,
                                   std::string* error = nullptr);
    [[nodiscard]] bool remove_rule(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool undo(std::string* error = nullptr);
    [[nodiscard]] bool redo(std::string* error = nullptr);
    [[nodiscard]] TileSetExternalChangeResult poll_external_change(bool force = false);

private:
    struct Snapshot { TileSet tileset; std::size_t selectedTile{}; };
    template<class Edit> [[nodiscard]] bool apply_edit(Edit&& edit, std::string* error);
    void remember_signature() noexcept;
    void normalize_selection() noexcept;

    TileSet tileset_;
    std::size_t selectedTile_{};
    std::filesystem::path path_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool dirty_{};
    std::filesystem::file_time_type writeTime_{};
    std::uintmax_t fileSize_{};
    bool signatureValid_{};
};

struct TileSetPaletteCell {
    std::size_t tileIndex{};
    std::uint32_t atlasIndex{};
    TileCanvasRect rect{};
    bool selected{};
};

struct TileSetPaletteFrame {
    TileCanvasRect viewport{};
    std::vector<TileSetPaletteCell> cells;
    float zoom{1.0F};
    TileVec2 pan{};
};

class TileSetPaletteWorkspace {
public:
    [[nodiscard]] TileSetAuthoringSession& session() noexcept { return session_; }
    [[nodiscard]] const TileSetAuthoringSession& session() const noexcept { return session_; }
    [[nodiscard]] TileSetPaletteFrame frame(TileCanvasRect viewport) const;
    [[nodiscard]] bool pointer_down(int button, TileVec2 point, TileCanvasRect viewport) noexcept;
    [[nodiscard]] bool pointer_move(TileVec2 point) noexcept;
    [[nodiscard]] bool pointer_up(int button) noexcept;
    [[nodiscard]] bool wheel(float steps, TileVec2 point, TileCanvasRect viewport) noexcept;

private:
    [[nodiscard]] TileVec2 screen_to_texture(TileVec2 point, TileCanvasRect viewport) const noexcept;
    TileSetAuthoringSession session_;
    float zoom_{1.0F};
    TileVec2 pan_{};
    bool panning_{};
    TileVec2 pointerDown_{};
    TileVec2 capturePan_{};
};

struct TileRegion {
    std::uint32_t minCol{};
    std::uint32_t minRow{};
    std::uint32_t maxCol{};
    std::uint32_t maxRow{};
};

struct TileEditImpact {
    bool changed{};
    bool completeMap{};
    bool collisionChanged{};
    bool semanticChanged{};
    std::optional<TileRegion> region;
};

struct TileMapAuthoringSelection {
    std::size_t tileLayer{};
    std::uint32_t tileValue{};
    std::string terrain;
    std::optional<std::pair<std::size_t, std::uint64_t>> object;
};

enum class TileMapExternalChange : std::uint8_t {
    Unchanged,
    Reloaded,
    Conflict,
    Missing,
    Failed,
};

struct TileMapExternalChangeResult {
    TileMapExternalChange state{TileMapExternalChange::Unchanged};
    std::string message;
};

// Transactional map document used by native, SDL, Qt, and headless level tools. Edits validate the
// complete canonical map before commit, retain bounded undo/redo history, and expose the exact tile
// region that physics and renderer caches need to refresh.
class TileMapAuthoringSession {
public:
    TileMapAuthoringSession() = default;

    [[nodiscard]] const TileMap& map() const noexcept { return map_; }
    [[nodiscard]] TileMapAuthoringSelection selection() const { return selection_; }
    [[nodiscard]] const TileEditImpact& last_impact() const noexcept { return lastImpact_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] bool create(std::string name, std::uint32_t width, std::uint32_t height,
                              TileSet tileset, std::string tilesetAsset = {},
                              std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path = {}, std::string* error = nullptr);
    [[nodiscard]] bool save_tileset(const std::filesystem::path& path,
                                    std::string* error = nullptr) const;
    [[nodiscard]] bool update_tileset_tile(std::size_t index, TileDef tile,
                                           std::string* error = nullptr);

    [[nodiscard]] bool select_tile_layer(std::size_t index) noexcept;
    [[nodiscard]] bool select_tile(std::uint32_t tileValue) noexcept;
    [[nodiscard]] bool select_terrain(std::string terrain) noexcept;
    [[nodiscard]] bool select_object(std::size_t layerIndex, std::uint64_t objectId) noexcept;
    void clear_object_selection() noexcept { selection_.object.reset(); }

    [[nodiscard]] bool add_tile_layer(std::string name, TileLayerKind kind,
                                      bool collidable, std::string* error = nullptr);
    [[nodiscard]] bool remove_tile_layer(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool add_object_layer(std::string name, std::string* error = nullptr);
    [[nodiscard]] bool remove_object_layer(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool update_tile_layer(std::size_t index, TileLayer layer,
                                         std::string* error = nullptr);
    [[nodiscard]] bool move_tile_layer(std::size_t index, std::size_t destination,
                                       std::string* error = nullptr);
    [[nodiscard]] bool update_object_layer(std::size_t index, TileObjectLayer layer,
                                           std::string* error = nullptr);
    [[nodiscard]] bool move_object_layer(std::size_t index, std::size_t destination,
                                         std::string* error = nullptr);

    [[nodiscard]] bool paint_cell(std::uint32_t col, std::uint32_t row,
                                  std::uint32_t tileValue, std::string* error = nullptr);
    [[nodiscard]] bool paint_rectangle(TileRegion region, std::uint32_t tileValue,
                                       std::string* error = nullptr);
    [[nodiscard]] bool flood_fill(std::uint32_t col, std::uint32_t row,
                                  std::uint32_t tileValue, std::string* error = nullptr);
    [[nodiscard]] bool paint_terrain(std::uint32_t col, std::uint32_t row,
                                     std::string terrain, bool erase,
                                     std::string* error = nullptr);
    [[nodiscard]] bool refresh_autotile(TileRegion region, std::string terrain,
                                        std::string* error = nullptr);

    [[nodiscard]] bool add_object(std::size_t layerIndex, TileObject object,
                                  std::uint64_t* assignedId = nullptr,
                                  std::string* error = nullptr);
    [[nodiscard]] bool update_object(std::size_t layerIndex, TileObject object,
                                     std::string* error = nullptr);
    [[nodiscard]] bool remove_object(std::size_t layerIndex, std::uint64_t objectId,
                                     std::string* error = nullptr);
    [[nodiscard]] bool set_object_property(std::size_t layerIndex, std::uint64_t objectId,
                                           std::string name, std::string value,
                                           std::string* error = nullptr);
    [[nodiscard]] bool update_object_shape(std::size_t layerIndex, std::uint64_t objectId,
                                           TileObjectShape shape, TileVec2 position,
                                           TileVec2 size, float rotationDegrees,
                                           std::string* error = nullptr);

    [[nodiscard]] bool undo(std::string* error = nullptr);
    [[nodiscard]] bool redo(std::string* error = nullptr);
    [[nodiscard]] TileMapExternalChangeResult poll_external_change(bool force = false);

private:
    struct Snapshot {
        TileMap map;
        TileMapAuthoringSelection selection;
    };

    template<class Edit>
    [[nodiscard]] bool apply_edit(Edit&& edit, std::string* error);
    void normalize_selection() noexcept;
    void remember_signature() noexcept;
    [[nodiscard]] std::uint64_t next_object_id() const noexcept;

    TileMap map_;
    TileMapAuthoringSelection selection_{};
    TileEditImpact lastImpact_{};
    std::filesystem::path path_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool dirty_{};
    std::filesystem::file_time_type writeTime_{};
    std::uintmax_t fileSize_{};
    bool signatureValid_{};
};

enum class TileMapTool : std::uint8_t {
    Pencil,
    Eraser,
    Rectangle,
    FloodFill,
    Terrain,
    Object,
    CollisionShape,
    Pan,
};

struct TileCanvasCellFrame {
    std::size_t layerIndex{};
    std::uint32_t col{};
    std::uint32_t row{};
    std::uint32_t tileValue{};
    std::uint32_t atlasIndex{};
    TileCanvasRect rect{};
    TileLayerKind kind{TileLayerKind::Visual};
    TileCollision collision{TileCollision::Empty};
    bool activeLayer{};
};

struct TileCanvasObjectFrame {
    std::size_t layerIndex{};
    std::uint64_t objectId{};
    TileCanvasRect rect{};
    TileObjectShape shape{TileObjectShape::Point};
    bool selected{};
    std::array<TileCanvasRect, 4> resizeHandles{}; // left, right, top, bottom
};

struct TileMapCanvasFrame {
    TileCanvasRect viewport{};
    std::vector<TileCanvasCellFrame> cells;
    std::vector<TileCanvasObjectFrame> objects;
    std::optional<TileRegion> brushPreview;
    float zoom{1.0F};
    TileVec2 pan{};
};

// Deterministic canvas interaction model. Hosts render the returned cells/objects and forward
// pointer/wheel events; all mutations remain in TileMapAuthoringSession.
class TileMapCanvasWorkspace {
public:
    [[nodiscard]] TileMapAuthoringSession& session() noexcept { return session_; }
    [[nodiscard]] const TileMapAuthoringSession& session() const noexcept { return session_; }
    [[nodiscard]] TileMapTool tool() const noexcept { return tool_; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] TileVec2 pan() const noexcept { return pan_; }

    void set_tool(TileMapTool tool) noexcept { tool_ = tool; }
    void set_pan(TileVec2 pan) noexcept { pan_ = pan; }
    [[nodiscard]] bool set_zoom(float zoom) noexcept;
    [[nodiscard]] TileMapCanvasFrame frame(TileCanvasRect viewport,
                                           std::uint64_t animationTicks = 0) const;

    [[nodiscard]] bool pointer_down(int button, TileVec2 point, TileCanvasRect viewport,
                                    std::string* error = nullptr);
    [[nodiscard]] bool pointer_move(TileVec2 point, TileCanvasRect viewport,
                                    std::string* error = nullptr);
    [[nodiscard]] bool pointer_up(int button, TileVec2 point, TileCanvasRect viewport,
                                  std::string* error = nullptr);
    [[nodiscard]] bool wheel(float steps, TileVec2 point, TileCanvasRect viewport) noexcept;

private:
    enum class Capture : std::uint8_t { NoCapture, Paint, Rectangle, Pan, ObjectMove, ObjectResize };
    enum class ResizeHandle : std::uint8_t { NoHandle, Left, Right, Top, Bottom };
    [[nodiscard]] TileVec2 screen_to_world(TileVec2 point, TileCanvasRect viewport) const noexcept;
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> screen_to_cell(
        TileVec2 point, TileCanvasRect viewport) const noexcept;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::uint64_t>> object_at(
        TileVec2 point, TileCanvasRect viewport) const noexcept;

    TileMapAuthoringSession session_;
    TileMapTool tool_{TileMapTool::Pencil};
    float zoom_{1.0F};
    TileVec2 pan_{};
    Capture capture_{Capture::NoCapture};
    TileVec2 pointerDown_{};
    TileVec2 capturePan_{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> startCell_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> currentCell_;
    std::optional<std::pair<std::size_t, std::uint64_t>> movingObject_;
    TileObject movingObjectStart_{};
    ResizeHandle resizeHandle_{ResizeHandle::NoHandle};
};

enum class TileWorldInspectorKind : std::uint8_t {
    NoSelection,
    Tile,
    Terrain,
    Object,
    Trigger,
    Hazard,
    Spawn,
};

enum class TileWorldDiagnosticSeverity : std::uint8_t { Information, Warning, Error };

struct TileWorldDiagnostic {
    TileWorldDiagnosticSeverity severity{TileWorldDiagnosticSeverity::Information};
    std::string location;
    std::string message;
};

struct TileWorldInspectorField {
    std::string name;
    std::string value;
    bool editable{true};
};

struct TileWorldInspectorFrame {
    TileWorldInspectorKind kind{TileWorldInspectorKind::NoSelection};
    std::string title;
    std::vector<TileWorldInspectorField> fields;
};

struct TileWorldLayerRow {
    bool objectLayer{};
    std::size_t index{};
    std::string name;
    TileLayerKind kind{TileLayerKind::Visual};
    bool visible{};
    bool locked{};
    bool collidable{};
    float parallaxX{1.0F};
    float parallaxY{1.0F};
    bool selected{};
};

struct TileAutotileRuleVisualization {
    std::size_t ruleIndex{};
    std::string terrain;
    std::array<std::int8_t, 9> neighborhood{}; // -1 forbidden, 0 ignored, 1 required, center=2
    std::uint32_t tileValue{};
    std::int32_t priority{};
    bool selected{};
};

struct TileParallaxLayerPreview {
    bool objectLayer{};
    std::size_t layerIndex{};
    std::string name;
    TileVec2 offset{};
    std::uint64_t visibleItems{};
};

struct TileRegionalRecookOverlay {
    TileRegion region{};
    bool collision{};
    bool renderer{};
    bool semantic{};
    std::uint64_t generation{};
};

struct TileWorldDesktopFrame {
    TileMapCanvasFrame canvas;
    TileSetPaletteFrame palette;
    std::vector<TileWorldLayerRow> layers;
    TileWorldInspectorFrame inspector;
    std::vector<TileAutotileRuleVisualization> autotileRules;
    std::vector<TileParallaxLayerPreview> parallaxLayers;
    std::vector<TileChunkDiagnostic> chunks;
    std::optional<TileRegionalRecookOverlay> recookOverlay;
    std::vector<TileWorldDiagnostic> validation;
    bool playRequested{};
    bool validForPlay{};
};

class TileWorldDesktopWorkspace {
public:
    [[nodiscard]] TileMapCanvasWorkspace& canvas() noexcept { return canvas_; }
    [[nodiscard]] const TileMapCanvasWorkspace& canvas() const noexcept { return canvas_; }
    [[nodiscard]] TileSetPaletteWorkspace& palette() noexcept { return palette_; }
    [[nodiscard]] const TileSetPaletteWorkspace& palette() const noexcept { return palette_; }
    [[nodiscard]] TileWorldInspectorKind inspector_kind() const noexcept { return inspectorKind_; }
    [[nodiscard]] bool show_autotile_rules() const noexcept { return showAutotileRules_; }
    [[nodiscard]] bool show_parallax_preview() const noexcept { return showParallaxPreview_; }
    [[nodiscard]] bool show_chunk_diagnostics() const noexcept { return showChunkDiagnostics_; }

    [[nodiscard]] bool open(const std::filesystem::path& mapPath,
                            const std::filesystem::path& tilesetPath = {},
                            std::string* error = nullptr);
    [[nodiscard]] bool save(std::string* error = nullptr);
    void set_inspector_kind(TileWorldInspectorKind kind) noexcept { inspectorKind_ = kind; }
    void set_show_autotile_rules(bool value) noexcept { showAutotileRules_ = value; }
    void set_show_parallax_preview(bool value) noexcept { showParallaxPreview_ = value; }
    void set_show_chunk_diagnostics(bool value) noexcept { showChunkDiagnostics_ = value; }
    void set_preview_camera(Camera2D camera) noexcept { previewCamera_ = camera; }

    [[nodiscard]] bool select_tile_layer(std::size_t index) noexcept;
    [[nodiscard]] bool select_object(std::size_t layerIndex, std::uint64_t objectId) noexcept;
    [[nodiscard]] bool canvas_pointer_down(int button, TileVec2 point, TileCanvasRect viewport,
                                           std::string* error = nullptr);
    [[nodiscard]] bool canvas_pointer_move(TileVec2 point, TileCanvasRect viewport,
                                           std::string* error = nullptr);
    [[nodiscard]] bool canvas_pointer_up(int button, TileVec2 point, TileCanvasRect viewport,
                                         std::string* error = nullptr);
    [[nodiscard]] bool canvas_wheel(float steps, TileVec2 point, TileCanvasRect viewport) noexcept;
    [[nodiscard]] bool palette_pointer_down(int button, TileVec2 point, TileCanvasRect viewport) noexcept;
    [[nodiscard]] bool palette_pointer_move(TileVec2 point) noexcept;
    [[nodiscard]] bool palette_pointer_up(int button) noexcept;
    [[nodiscard]] bool palette_wheel(float steps, TileVec2 point, TileCanvasRect viewport) noexcept;
    [[nodiscard]] bool update_selected_tile(TileDef tile, std::string* error = nullptr);
    [[nodiscard]] bool update_selected_tile_layer(TileLayer layer, std::string* error = nullptr);
    [[nodiscard]] bool update_selected_object(TileObject object, std::string* error = nullptr);
    [[nodiscard]] bool update_selected_object_layer(TileObjectLayer layer,
                                                    std::string* error = nullptr);
    [[nodiscard]] bool resize_selected_collision_shape(TileVec2 minimum, TileVec2 size,
                                                       float rotationDegrees,
                                                       std::string* error = nullptr);
    [[nodiscard]] bool place_prefab(std::size_t objectLayer, std::string prefabAsset,
                                    TileVec2 position,
                                    std::vector<TileObjectProperty> overrides = {},
                                    std::uint64_t* assignedId = nullptr,
                                    std::string* error = nullptr);
    [[nodiscard]] bool set_prefab_override(std::string name, std::string value,
                                           std::string* error = nullptr);
    [[nodiscard]] std::vector<TileWorldDiagnostic> validate_level() const;
    [[nodiscard]] bool request_play_test(std::string* error = nullptr);
    [[nodiscard]] bool consume_play_request() noexcept;
    [[nodiscard]] TileWorldDesktopFrame frame(TileCanvasRect canvasViewport,
                                              TileCanvasRect paletteViewport,
                                              std::uint64_t animationTicks = 0U) const;

private:
    [[nodiscard]] TileWorldInspectorFrame build_inspector() const;
    [[nodiscard]] std::vector<TileWorldLayerRow> build_layers() const;
    [[nodiscard]] std::vector<TileAutotileRuleVisualization> build_autotile_rules() const;
    [[nodiscard]] std::vector<TileParallaxLayerPreview> build_parallax_preview(
        std::uint64_t animationTicks) const;
    void record_recook_overlay() noexcept;

    TileMapCanvasWorkspace canvas_;
    TileSetPaletteWorkspace palette_;
    TileWorldInspectorKind inspectorKind_{TileWorldInspectorKind::Tile};
    Camera2D previewCamera_{};
    bool showAutotileRules_{true};
    bool showParallaxPreview_{true};
    bool showChunkDiagnostics_{true};
    bool playRequested_{};
    std::uint64_t recookGeneration_{};
    std::optional<TileRegionalRecookOverlay> recookOverlay_;
};

} // namespace dve
