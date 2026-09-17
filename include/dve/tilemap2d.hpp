#pragma once

// Side-scroller foundation: tilemaps, a deterministic tile-based collision grid with swept AABB
// resolution (solid tiles and one-way platforms), a follow camera with a dead zone and clamped
// scroll bounds, and a parallax layer model. This is the Milestone B core from the v1.94 plan.
//
// The module is self-contained (standard library only) so the collision math, serialization, and
// camera logic are validated on the CPU without a renderer. It composes with dve/sprite2d.hpp: a
// tile is drawn as a sprite from an atlas, and the camera center feeds snap_world_to_pixel for
// crisp integer-scaled presentation. Coordinates are pixels in a y-down space (row 0 at the top,
// gravity pulling +y), which matches how tile grids are authored and how sprites are laid out.
//
// Everything here is deterministic and allocation-light on the hot paths (move_aabb performs no
// heap allocation).

#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>
#include <string_view>
#include <vector>

namespace dve {

struct TileVec2 {
    float x{};
    float y{};
};

// Axis-aligned box stored as a minimum corner plus a size, in pixels.
struct TileAabb {
    TileVec2 min{};
    TileVec2 size{};

    [[nodiscard]] float max_x() const noexcept { return min.x + size.x; }
    [[nodiscard]] float max_y() const noexcept { return min.y + size.y; }
    [[nodiscard]] TileVec2 center() const noexcept { return {min.x + size.x * 0.5F, min.y + size.y * 0.5F}; }
};

// Per-tile collision behavior. Empty is passable; Solid blocks from every direction; OneWayTop is a
// platform that only blocks a downward-moving mover landing on its top surface (jump up through it).
enum class TileCollision : std::uint8_t {
    Empty,
    Solid,
    OneWayTop,
    // Floor ramps in the y-down gameplay plane. "Up" names the direction in which the
    // surface rises: UpRight is low on the left and high on the right; UpLeft is the inverse.
    SlopeUpRight,
    SlopeUpLeft
};

// Authoring/runtime purpose of a tile layer. The kind is descriptive; collidable remains the
// authoritative collision switch so legacy maps and custom mixed-purpose layers keep working.
enum class TileLayerKind : std::uint8_t {
    Visual,
    Collision,
    Hazard,
    Trigger
};

// Four-neighbor autotile mask bits. Diagonal masks can be added in a future format without
// changing the deterministic v2 rule ordering.
inline constexpr std::uint8_t kTileNeighborNorth = 1U << 0U;
inline constexpr std::uint8_t kTileNeighborEast  = 1U << 1U;
inline constexpr std::uint8_t kTileNeighborSouth = 1U << 2U;
inline constexpr std::uint8_t kTileNeighborWest  = 1U << 3U;

struct TileAutotileRule {
    std::string terrain;
    std::uint8_t requiredMask{};
    std::uint8_t forbiddenMask{};
    std::uint32_t tileValue{}; // 1-based tile id plus optional flip flags
    std::int32_t priority{};
};

// High bits packed into a layer tile value for rendering flips; low bits are the 1-based tile id
// (0 means an empty cell).
inline constexpr std::uint32_t kTileFlipX = 0x80000000U;
inline constexpr std::uint32_t kTileFlipY = 0x40000000U;
inline constexpr std::uint32_t kTileIdMask = 0x3FFFFFFFU;

[[nodiscard]] constexpr std::uint32_t tile_id(std::uint32_t value) noexcept { return value & kTileIdMask; }
[[nodiscard]] constexpr bool tile_flip_x(std::uint32_t value) noexcept { return (value & kTileFlipX) != 0U; }
[[nodiscard]] constexpr bool tile_flip_y(std::uint32_t value) noexcept { return (value & kTileFlipY) != 0U; }

// A tile definition inside the set: which atlas cell to draw and how it collides.
struct TileDef {
    std::string name;
    std::uint32_t atlasIndex{0};
    TileCollision collision{TileCollision::Empty};

    // v2 semantic metadata. Identifiers are whitespace-free canonical tokens; an empty string
    // means the behavior is absent. Existing aggregate initializers remain source-compatible.
    std::string terrain;
    std::string material;
    float damagePerSecond{};
    TileVec2 conveyorVelocity{};
    bool ladder{};
    std::string trigger;

    // Contiguous atlas animation. A count <= 1 is static. ticksPerFrame is measured in the
    // caller's deterministic tile clock and must be positive for animated tiles.
    std::uint32_t animationFirstAtlasIndex{};
    std::uint32_t animationFrameCount{1};
    std::uint32_t animationTicksPerFrame{};
};

struct TileSet {
    std::string name;
    std::string textureAsset;
    std::uint32_t textureWidth{};
    std::uint32_t textureHeight{};
    std::uint32_t tileWidth{16};
    std::uint32_t tileHeight{16};
    std::uint32_t margin{};            // source texture border before the first tile
    std::uint32_t spacing{};           // source texture gap between adjacent tiles
    std::uint32_t columns{16};         // atlas columns, for atlasIndex -> uv mapping
    std::vector<TileDef> tiles;        // index 0 is tile id 1 (ids are 1-based in layers)
    std::vector<TileAutotileRule> autotileRules;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] TileCollision collision_for(std::uint32_t tileValue) const noexcept;
    [[nodiscard]] std::uint32_t atlas_index_for(std::uint32_t tileValue,
                                                std::uint64_t ticks = 0) const noexcept;
    [[nodiscard]] std::uint32_t autotile_value(std::string_view terrain,
                                               std::uint8_t neighborMask) const noexcept;

    // Reusable canonical .dvetileset asset. Maps may reference one while embedding the resolved
    // copy needed for deterministic runtime loading and physics cooking.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static bool parse(std::string_view text, TileSet& out,
                                    std::string* error = nullptr);
    [[nodiscard]] std::uint64_t content_hash() const;
};

struct TileLayer {
    std::string name;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint32_t> tiles;  // size == width*height, row-major, 0 == empty
    float parallaxX{1.0F};             // 1.0 scrolls with the world; <1 is a distant background
    float parallaxY{1.0F};
    bool visible{true};
    bool collidable{true};
    bool locked{false};
    TileLayerKind kind{TileLayerKind::Visual};

    [[nodiscard]] std::uint32_t at(std::uint32_t col, std::uint32_t row) const noexcept;
    void set(std::uint32_t col, std::uint32_t row, std::uint32_t value) noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

enum class TileObjectShape : std::uint8_t {
    Point,
    Rectangle,
    Ellipse
};

struct TileObjectProperty {
    std::string name;
    std::string value;
};

struct TileObject {
    std::uint64_t id{};
    std::string name;
    std::string type;
    TileObjectShape shape{TileObjectShape::Point};
    TileVec2 position{};
    TileVec2 size{};
    float rotationDegrees{};
    std::vector<TileObjectProperty> properties;
};

struct TileObjectLayer {
    std::string name;
    float parallaxX{1.0F};
    float parallaxY{1.0F};
    bool visible{true};
    bool locked{false};
    std::vector<TileObject> objects;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct TileMap {
    std::string name;
    std::uint32_t tileWidth{16};
    std::uint32_t tileHeight{16};
    std::string tilesetAsset;
    TileSet tileset;
    std::vector<TileLayer> layers;
    std::vector<TileObjectLayer> objectLayers;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::uint32_t pixel_width() const noexcept;   // widest layer, in pixels
    [[nodiscard]] std::uint32_t pixel_height() const noexcept;

    // Canonical, deterministic .dvetilemap v2 text. v1 remains readable; serialize() emits v2.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static bool parse(std::string_view text, TileMap& out, std::string* error = nullptr);
    [[nodiscard]] std::uint64_t content_hash() const;
};



struct TileCellHit {
    std::size_t layerIndex{};
    std::uint32_t col{};
    std::uint32_t row{};
    std::uint32_t tileValue{};
    TileCollision collision{TileCollision::Empty};
    std::string material;
    float damagePerSecond{};
    TileVec2 conveyorVelocity{};
    bool ladder{};
    std::string trigger;
};

struct TileObjectHit {
    std::size_t layerIndex{};
    std::uint64_t objectId{};
    std::string type;
    std::string name;
};

struct TileWorldQuery {
    std::vector<TileCellHit> cells;
    std::vector<TileObjectHit> objects;
    float totalDamagePerSecond{};
    TileVec2 conveyorVelocity{};
    bool onLadder{};
    std::vector<std::string> triggers;
};

// Queries all semantic tile cells and objects overlapping a world-space AABB. Ordering is stable:
// tile layers, rows, columns, then object layers and declaration order.
[[nodiscard]] TileWorldQuery query_tile_world(const TileMap& map, const TileAabb& bounds);

struct TileRenderItem {
    std::size_t layerIndex{};
    std::uint32_t col{};
    std::uint32_t row{};
    std::uint32_t tileValue{};
    std::uint32_t atlasIndex{};
    TileAabb worldBounds{};
    float parallaxX{1.0F};
    float parallaxY{1.0F};
    bool flipX{};
    bool flipY{};
};

// Stable visible-tile extraction for renderers and editor previews. It resolves animated atlas
// frames but remains independent of a particular sprite/RHI backend.
[[nodiscard]] std::vector<TileRenderItem> build_tile_render_list(
    const TileMap& map, const TileAabb& worldView, std::uint64_t animationTicks = 0);

struct TileChunkDiagnostic {
    std::size_t layerIndex{};
    std::uint32_t chunkCol{};
    std::uint32_t chunkRow{};
    std::uint32_t nonEmptyTiles{};
    std::uint32_t collidableTiles{};
    std::uint32_t hazardTiles{};
    std::uint32_t triggerTiles{};
    std::uint32_t objectCount{};
    std::uint64_t contentHash{};
};

// Deterministic chunk inventory used by editor diagnostics, streaming plans, and regional recook
// telemetry. Object counts are accumulated into matching world-space chunks after tile layers.
[[nodiscard]] std::vector<TileChunkDiagnostic> build_tile_chunk_diagnostics(
    const TileMap& map, std::uint32_t chunkWidth = 16U, std::uint32_t chunkHeight = 16U);

// A flattened collision field merged from every collidable layer, for fast swept queries. Solid
// wins over OneWayTop when layers overlap. Cells outside the grid read as Empty (open world).
struct CollisionGrid {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t tileWidth{16};
    std::uint32_t tileHeight{16};
    std::vector<TileCollision> cells;

    [[nodiscard]] TileCollision at(std::int64_t col, std::int64_t row) const noexcept;
    void set(std::uint32_t col, std::uint32_t row, TileCollision collision) noexcept;
};

[[nodiscard]] CollisionGrid build_collision_grid(const TileMap& map);

// Rebuild only the requested inclusive tile rectangle. The grid dimensions and tile size must
// already match the map. This is used by live tile editing and destructible 2D levels without
// rebuilding the complete collision field.
[[nodiscard]] bool update_collision_grid_region(CollisionGrid& grid, const TileMap& map,
                                                std::uint32_t minCol, std::uint32_t minRow,
                                                std::uint32_t maxCol, std::uint32_t maxRow) noexcept;

// Result of sweeping an AABB through the collision grid.
struct AabbMoveOptions {
    // Used by character controllers for intentional drop-through movement. Solid tiles still block.
    bool ignoreOneWayPlatforms{false};
};

struct AabbSweep {
    TileVec2 position{};      // resolved minimum corner
    TileVec2 velocity{};      // velocity after resolution (blocked axes zeroed)
    bool hitLeft{false};
    bool hitRight{false};
    bool hitTop{false};
    bool hitBottom{false};
    bool onGround{false};     // landed on a solid, one-way, or slope surface this move
    TileVec2 groundNormal{0.0F, -1.0F};
};

// Move an AABB (min corner + size) by displacement, resolving against solid and one-way tiles.
// Axis-separated (x then y) for stable platformer behavior. displacement is the intended motion in
// pixels for this step. No heap allocation.
[[nodiscard]] AabbSweep move_aabb(const CollisionGrid& grid, const TileAabb& box,
                                  TileVec2 displacement, AabbMoveOptions options = {}) noexcept;

// A 2D scrolling camera. The camera keeps the follow target inside a dead zone (a centered box, in
// pixels) and clamps its view to the world bounds so the edges never scroll past the level.
struct Camera2D {
    TileVec2 center{};        // world-space center of the view, in pixels
    float viewWidth{320.0F};
    float viewHeight{180.0F};
    TileVec2 deadZoneHalf{16.0F, 12.0F}; // half-extents of the dead zone around the center
    TileVec2 worldMin{0.0F, 0.0F};
    TileVec2 worldMax{0.0F, 0.0F};       // set worldMax > worldMin to enable clamping

    [[nodiscard]] TileAabb view_bounds() const noexcept; // world-space rect currently visible
};

// Move the camera so the target stays within the dead zone, then clamp to the world bounds.
void camera_follow(Camera2D& camera, TileVec2 target) noexcept;

// Set worldMin/worldMax from a map so the camera clamps to the level edges.
void camera_set_world_from_map(Camera2D& camera, const TileMap& map) noexcept;

// The scroll offset to render a parallax layer at, given the camera. A factor of 1 tracks the
// world exactly; smaller factors lag for depth. The returned value is the world-space position that
// the layer's origin should be drawn relative to the camera's top-left.
[[nodiscard]] TileVec2 parallax_layer_origin(const Camera2D& camera, float parallaxX, float parallaxY) noexcept;

} // namespace dve
