#include "dve/tilemap_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <queue>
#include <set>
#include <system_error>

namespace dve {
namespace {

constexpr std::size_t kMaxUndo = 128U;
constexpr std::uintmax_t kMaxTileMapFileBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaxCanvasCells = 250000U;

[[nodiscard]] bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

[[nodiscard]] bool write_text_recoverable(const std::filesystem::path& path,
                                           std::string_view text,
                                           std::string* error) {
    if (path.empty()) return fail(error, "output path is empty");
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create output directory: " + ec.message());
    const std::filesystem::path temporary = path.string() + ".tmp";
    const std::filesystem::path backup = path.string() + ".bak";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary output");
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) return fail(error, "could not write temporary output");
    }
    std::filesystem::remove(backup, ec);
    ec.clear();
    const bool hadOriginal = std::filesystem::exists(path, ec) && !ec;
    if (hadOriginal) {
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            std::filesystem::remove(temporary);
            return fail(error, "could not stage existing output: " + ec.message());
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (hadOriginal) {
            std::error_code rollback;
            std::filesystem::rename(backup, path, rollback);
        }
        std::filesystem::remove(temporary);
        return fail(error, "could not publish output: " + ec.message());
    }
    if (hadOriginal) std::filesystem::remove(backup, ec);
    return true;
}

[[nodiscard]] bool read_text(const std::filesystem::path& path, std::string& text,
                             std::string* error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail(error, "could not inspect tile-map file: " + ec.message());
    if (size > kMaxTileMapFileBytes) return fail(error, "tile-map file exceeds 256 MiB limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(error, "could not open tile-map file");
    text.resize(static_cast<std::size_t>(size));
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream && !text.empty()) return fail(error, "could not read tile-map file");
    return true;
}

[[nodiscard]] TileRegion normalized_region(TileRegion region) noexcept {
    if (region.minCol > region.maxCol) std::swap(region.minCol, region.maxCol);
    if (region.minRow > region.maxRow) std::swap(region.minRow, region.maxRow);
    return region;
}

[[nodiscard]] bool valid_region(const TileLayer& layer, TileRegion region) noexcept {
    return region.minCol <= region.maxCol && region.minRow <= region.maxRow &&
        region.maxCol < layer.width && region.maxRow < layer.height;
}

[[nodiscard]] bool layer_affects_collision(const TileLayer& layer) noexcept {
    return layer.collidable || layer.kind == TileLayerKind::Collision;
}

[[nodiscard]] bool layer_affects_semantics(const TileLayer& layer) noexcept {
    return layer.kind == TileLayerKind::Hazard || layer.kind == TileLayerKind::Trigger;
}

[[nodiscard]] std::string_view terrain_at(const TileMap& map, const TileLayer& layer,
                                          std::int64_t col, std::int64_t row) noexcept {
    if (col < 0 || row < 0 || col >= static_cast<std::int64_t>(layer.width) ||
        row >= static_cast<std::int64_t>(layer.height)) return {};
    const std::uint32_t id = tile_id(layer.at(static_cast<std::uint32_t>(col),
                                              static_cast<std::uint32_t>(row)));
    if (id == 0U || id > map.tileset.tiles.size()) return {};
    return map.tileset.tiles[id - 1U].terrain;
}

[[nodiscard]] std::uint8_t neighbor_mask(const TileMap& map, const TileLayer& layer,
                                         std::uint32_t col, std::uint32_t row,
                                         std::string_view terrain) noexcept {
    std::uint8_t mask = 0U;
    if (terrain_at(map, layer, col, static_cast<std::int64_t>(row) - 1) == terrain)
        mask |= kTileNeighborNorth;
    if (terrain_at(map, layer, static_cast<std::int64_t>(col) + 1, row) == terrain)
        mask |= kTileNeighborEast;
    if (terrain_at(map, layer, col, static_cast<std::int64_t>(row) + 1) == terrain)
        mask |= kTileNeighborSouth;
    if (terrain_at(map, layer, static_cast<std::int64_t>(col) - 1, row) == terrain)
        mask |= kTileNeighborWest;
    return mask;
}

void refresh_autotile_in_map(TileMap& map, std::size_t layerIndex, TileRegion region,
                             std::string_view terrain) {
    TileLayer& layer = map.layers[layerIndex];
    region = normalized_region(region);
    region.minCol = region.minCol > 0U ? region.minCol - 1U : 0U;
    region.minRow = region.minRow > 0U ? region.minRow - 1U : 0U;
    region.maxCol = std::min(layer.width - 1U, region.maxCol + 1U);
    region.maxRow = std::min(layer.height - 1U, region.maxRow + 1U);
    struct Replacement { std::uint32_t col; std::uint32_t row; std::uint32_t value; };
    std::vector<Replacement> replacements;
    for (std::uint32_t row = region.minRow; row <= region.maxRow; ++row) {
        for (std::uint32_t col = region.minCol; col <= region.maxCol; ++col) {
            if (terrain_at(map, layer, col, row) != terrain) continue;
            const std::uint8_t mask = neighbor_mask(map, layer, col, row, terrain);
            const std::uint32_t value = map.tileset.autotile_value(terrain, mask);
            if (value != 0U && value != layer.at(col, row)) replacements.push_back({col, row, value});
        }
    }
    for (const Replacement& replacement : replacements)
        layer.set(replacement.col, replacement.row, replacement.value);
}

[[nodiscard]] bool object_overlap(const TileObject& object, TileVec2 world) noexcept {
    if (object.shape == TileObjectShape::Point)
        return std::fabs(world.x - object.position.x) <= 5.0F &&
            std::fabs(world.y - object.position.y) <= 5.0F;
    return world.x >= object.position.x && world.y >= object.position.y &&
        world.x <= object.position.x + object.size.x &&
        world.y <= object.position.y + object.size.y;
}

} // namespace


template<class Edit>
bool TileSetAuthoringSession::apply_edit(Edit&& edit, std::string* error) {
    TileSet candidate = tileset_;
    std::size_t candidateSelection = selectedTile_;
    if (!edit(candidate, candidateSelection, error)) return false;
    std::string validation;
    if (!candidate.validate(&validation)) return fail(error, validation);
    if (candidate.serialize() == tileset_.serialize() && candidateSelection == selectedTile_) return true;
    if (undo_.size() == kMaxUndo) undo_.erase(undo_.begin());
    undo_.push_back({tileset_, selectedTile_});
    redo_.clear();
    tileset_ = std::move(candidate);
    selectedTile_ = candidateSelection;
    normalize_selection();
    dirty_ = true;
    return true;
}

bool TileSetAuthoringSession::create(std::string name, std::string textureAsset,
                                     std::uint32_t textureWidth, std::uint32_t textureHeight,
                                     std::uint32_t tileWidth, std::uint32_t tileHeight,
                                     std::uint32_t margin, std::uint32_t spacing,
                                     std::string* error) {
    if (textureWidth == 0U || textureHeight == 0U || tileWidth == 0U || tileHeight == 0U)
        return fail(error, "tileset texture and tile dimensions must be positive");
    const std::uint64_t border = static_cast<std::uint64_t>(margin) * 2U;
    if (border + tileWidth > textureWidth || border + tileHeight > textureHeight)
        return fail(error, "tileset margin leaves no complete source tile");
    const std::uint64_t strideX = static_cast<std::uint64_t>(tileWidth) + spacing;
    const std::uint64_t strideY = static_cast<std::uint64_t>(tileHeight) + spacing;
    const std::uint64_t usableWidth = textureWidth - border;
    const std::uint64_t usableHeight = textureHeight - border;
    const std::uint64_t columns = 1U + (usableWidth - tileWidth) / strideX;
    const std::uint64_t rows = 1U + (usableHeight - tileHeight) / strideY;
    const std::uint64_t count = columns * rows;
    if (columns == 0U || rows == 0U || count == 0U || count > kTileIdMask ||
        count > std::numeric_limits<std::size_t>::max()) {
        return fail(error, "tileset slice grid exceeds supported tile count");
    }

    TileSet candidate;
    candidate.name = std::move(name);
    candidate.textureAsset = std::move(textureAsset);
    candidate.textureWidth = textureWidth;
    candidate.textureHeight = textureHeight;
    candidate.tileWidth = tileWidth;
    candidate.tileHeight = tileHeight;
    candidate.margin = margin;
    candidate.spacing = spacing;
    candidate.columns = static_cast<std::uint32_t>(columns);
    candidate.tiles.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        TileDef tile;
        tile.name = "tile_" + std::to_string(index + 1U);
        tile.atlasIndex = static_cast<std::uint32_t>(index);
        tile.animationFirstAtlasIndex = tile.atlasIndex;
        candidate.tiles.push_back(std::move(tile));
    }
    std::string validation;
    if (!candidate.validate(&validation)) return fail(error, validation);
    tileset_ = std::move(candidate);
    selectedTile_ = 0U;
    path_.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = true;
    signatureValid_ = false;
    return true;
}

bool TileSetAuthoringSession::open(const std::filesystem::path& path, std::string* error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    TileSet parsed;
    if (!TileSet::parse(text, parsed, error)) return false;
    tileset_ = std::move(parsed);
    selectedTile_ = 0U;
    path_ = path;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    remember_signature();
    return true;
}

bool TileSetAuthoringSession::save(const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path destination = path.empty() ? path_ : path;
    std::string validation;
    if (!tileset_.validate(&validation)) return fail(error, validation);
    if (!write_text_recoverable(destination, tileset_.serialize(), error)) return false;
    path_ = destination;
    dirty_ = false;
    remember_signature();
    return true;
}

bool TileSetAuthoringSession::select_tile(std::size_t index) noexcept {
    if (index >= tileset_.tiles.size()) return false;
    selectedTile_ = index;
    return true;
}

bool TileSetAuthoringSession::update_tile(std::size_t index, TileDef tile,
                                          std::string* error) {
    return apply_edit([&](TileSet& candidate, std::size_t& selection,
                          std::string* localError) {
        if (index >= candidate.tiles.size()) return fail(localError, "tile index is out of range");
        candidate.tiles[index] = std::move(tile);
        selection = index;
        return true;
    }, error);
}

bool TileSetAuthoringSession::remove_tile(std::size_t index, std::string* error) {
    return apply_edit([&](TileSet& candidate, std::size_t& selection,
                          std::string* localError) {
        if (index >= candidate.tiles.size()) return fail(localError, "tile index is out of range");
        const std::uint32_t removedId = static_cast<std::uint32_t>(index + 1U);
        candidate.tiles.erase(candidate.tiles.begin() + static_cast<std::ptrdiff_t>(index));
        std::erase_if(candidate.autotileRules, [&](const TileAutotileRule& rule) {
            return tile_id(rule.tileValue) == removedId;
        });
        for (TileAutotileRule& rule : candidate.autotileRules) {
            const std::uint32_t id = tile_id(rule.tileValue);
            if (id > removedId) {
                const std::uint32_t flags = rule.tileValue & ~kTileIdMask;
                rule.tileValue = flags | (id - 1U);
            }
        }
        if (selection > index) --selection;
        else if (selection == index && selection >= candidate.tiles.size() && selection > 0U) --selection;
        return true;
    }, error);
}

bool TileSetAuthoringSession::add_rule(TileAutotileRule rule, std::string* error) {
    return apply_edit([&](TileSet& candidate, std::size_t&, std::string*) {
        candidate.autotileRules.push_back(std::move(rule));
        return true;
    }, error);
}

bool TileSetAuthoringSession::update_rule(std::size_t index, TileAutotileRule rule,
                                          std::string* error) {
    return apply_edit([&](TileSet& candidate, std::size_t&, std::string* localError) {
        if (index >= candidate.autotileRules.size())
            return fail(localError, "autotile rule index is out of range");
        candidate.autotileRules[index] = std::move(rule);
        return true;
    }, error);
}

bool TileSetAuthoringSession::remove_rule(std::size_t index, std::string* error) {
    return apply_edit([&](TileSet& candidate, std::size_t&, std::string* localError) {
        if (index >= candidate.autotileRules.size())
            return fail(localError, "autotile rule index is out of range");
        candidate.autotileRules.erase(candidate.autotileRules.begin() +
                                      static_cast<std::ptrdiff_t>(index));
        return true;
    }, error);
}

bool TileSetAuthoringSession::undo(std::string* error) {
    if (undo_.empty()) return fail(error, "nothing to undo");
    if (redo_.size() == kMaxUndo) redo_.erase(redo_.begin());
    redo_.push_back({tileset_, selectedTile_});
    Snapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    tileset_ = std::move(snapshot.tileset);
    selectedTile_ = snapshot.selectedTile;
    normalize_selection();
    dirty_ = true;
    return true;
}

bool TileSetAuthoringSession::redo(std::string* error) {
    if (redo_.empty()) return fail(error, "nothing to redo");
    if (undo_.size() == kMaxUndo) undo_.erase(undo_.begin());
    undo_.push_back({tileset_, selectedTile_});
    Snapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    tileset_ = std::move(snapshot.tileset);
    selectedTile_ = snapshot.selectedTile;
    normalize_selection();
    dirty_ = true;
    return true;
}

TileSetExternalChangeResult TileSetAuthoringSession::poll_external_change(bool force) {
    TileSetExternalChangeResult result;
    if (path_.empty()) return result;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        result.state = TileSetExternalChange::Missing;
        result.message = "Tileset file is missing or unreadable";
        return result;
    }
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) {
        result.state = TileSetExternalChange::Missing;
        result.message = "Tileset file size is unavailable";
        return result;
    }
    if (!force && signatureValid_ && time == writeTime_ && size == fileSize_) return result;
    if (dirty_) {
        result.state = TileSetExternalChange::Conflict;
        result.message = "Tileset changed on disk while local edits are unsaved";
        return result;
    }
    std::string text;
    std::string parseError;
    TileSet parsed;
    if (!read_text(path_, text, &parseError) || !TileSet::parse(text, parsed, &parseError)) {
        result.state = TileSetExternalChange::Failed;
        result.message = parseError;
        return result;
    }
    tileset_ = std::move(parsed);
    selectedTile_ = 0U;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    writeTime_ = time;
    fileSize_ = size;
    signatureValid_ = true;
    result.state = TileSetExternalChange::Reloaded;
    result.message = "Reloaded changed tileset asset";
    return result;
}

void TileSetAuthoringSession::remember_signature() noexcept {
    signatureValid_ = false;
    if (path_.empty()) return;
    std::error_code ec;
    writeTime_ = std::filesystem::last_write_time(path_, ec);
    if (ec) return;
    fileSize_ = std::filesystem::file_size(path_, ec);
    signatureValid_ = !ec;
}

void TileSetAuthoringSession::normalize_selection() noexcept {
    if (tileset_.tiles.empty()) selectedTile_ = 0U;
    else if (selectedTile_ >= tileset_.tiles.size()) selectedTile_ = tileset_.tiles.size() - 1U;
}

TileVec2 TileSetPaletteWorkspace::screen_to_texture(TileVec2 point,
                                                    TileCanvasRect viewport) const noexcept {
    const float inverse = zoom_ > 0.0001F ? 1.0F / zoom_ : 1.0F;
    return {(point.x - viewport.x - pan_.x) * inverse,
            (point.y - viewport.y - pan_.y) * inverse};
}

TileSetPaletteFrame TileSetPaletteWorkspace::frame(TileCanvasRect viewport) const {
    TileSetPaletteFrame result;
    result.viewport = viewport;
    result.zoom = zoom_;
    result.pan = pan_;
    const TileSet& set = session_.tileset();
    if (set.tileWidth == 0U || set.tileHeight == 0U || set.columns == 0U) return result;
    result.cells.reserve(set.tiles.size());
    for (std::size_t index = 0U; index < set.tiles.size(); ++index) {
        const std::uint32_t atlas = set.tiles[index].atlasIndex;
        const std::uint32_t col = atlas % set.columns;
        const std::uint32_t row = atlas / set.columns;
        const float sourceX = static_cast<float>(set.margin +
            col * (set.tileWidth + set.spacing));
        const float sourceY = static_cast<float>(set.margin +
            row * (set.tileHeight + set.spacing));
        const TileCanvasRect rect{
            viewport.x + pan_.x + sourceX * zoom_,
            viewport.y + pan_.y + sourceY * zoom_,
            static_cast<float>(set.tileWidth) * zoom_,
            static_cast<float>(set.tileHeight) * zoom_};
        if (rect.x + rect.width < viewport.x || rect.y + rect.height < viewport.y ||
            rect.x > viewport.x + viewport.width || rect.y > viewport.y + viewport.height) continue;
        result.cells.push_back({index, atlas, rect, index == session_.selected_tile()});
    }
    return result;
}

bool TileSetPaletteWorkspace::pointer_down(int button, TileVec2 point,
                                           TileCanvasRect viewport) noexcept {
    pointerDown_ = point;
    if (button == 2 || button == 3) {
        panning_ = true;
        capturePan_ = pan_;
        return true;
    }
    if (button != 1 || !viewport.contains(point)) return false;
    const TileSetPaletteFrame current = frame(viewport);
    for (auto it = current.cells.rbegin(); it != current.cells.rend(); ++it) {
        if (it->rect.contains(point)) return session_.select_tile(it->tileIndex);
    }
    return false;
}

bool TileSetPaletteWorkspace::pointer_move(TileVec2 point) noexcept {
    if (!panning_) return false;
    pan_ = {capturePan_.x + point.x - pointerDown_.x,
            capturePan_.y + point.y - pointerDown_.y};
    return true;
}

bool TileSetPaletteWorkspace::pointer_up(int button) noexcept {
    if (button != 2 && button != 3) return false;
    const bool changed = panning_;
    panning_ = false;
    return changed;
}

bool TileSetPaletteWorkspace::wheel(float steps, TileVec2 point,
                                    TileCanvasRect viewport) noexcept {
    if (!std::isfinite(steps) || !viewport.contains(point)) return false;
    const TileVec2 textureBefore = screen_to_texture(point, viewport);
    zoom_ = std::clamp(zoom_ * std::pow(1.125F, steps), 0.125F, 16.0F);
    pan_.x = point.x - viewport.x - textureBefore.x * zoom_;
    pan_.y = point.y - viewport.y - textureBefore.y * zoom_;
    return true;
}

template<class Edit>
bool TileMapAuthoringSession::apply_edit(Edit&& edit, std::string* error) {
    TileMap candidate = map_;
    TileMapAuthoringSelection candidateSelection = selection_;
    TileEditImpact impact;
    if (!edit(candidate, candidateSelection, impact, error)) return false;
    if (!impact.changed) {
        lastImpact_ = impact;
        return true;
    }
    std::string validation;
    if (!candidate.validate(&validation)) return fail(error, validation);
    if (undo_.size() == kMaxUndo) undo_.erase(undo_.begin());
    undo_.push_back({map_, selection_});
    redo_.clear();
    map_ = std::move(candidate);
    selection_ = std::move(candidateSelection);
    normalize_selection();
    lastImpact_ = impact;
    dirty_ = true;
    return true;
}

bool TileMapAuthoringSession::create(std::string name, std::uint32_t width,
                                     std::uint32_t height, TileSet tileset,
                                     std::string tilesetAsset, std::string* error) {
    if (width == 0U || height == 0U) return fail(error, "map dimensions must be positive");
    TileMap candidate;
    candidate.name = std::move(name);
    candidate.tileWidth = tileset.tileWidth;
    candidate.tileHeight = tileset.tileHeight;
    candidate.tilesetAsset = std::move(tilesetAsset);
    candidate.tileset = std::move(tileset);
    TileLayer visual;
    visual.name = "Visual";
    visual.width = width;
    visual.height = height;
    visual.tiles.assign(static_cast<std::size_t>(width) * height, 0U);
    visual.collidable = false;
    visual.kind = TileLayerKind::Visual;
    candidate.layers.push_back(std::move(visual));
    TileLayer collision;
    collision.name = "Collision";
    collision.width = width;
    collision.height = height;
    collision.tiles.assign(static_cast<std::size_t>(width) * height, 0U);
    collision.visible = true;
    collision.collidable = true;
    collision.kind = TileLayerKind::Collision;
    candidate.layers.push_back(std::move(collision));
    TileObjectLayer objectLayer;
    objectLayer.name = "Objects";
    candidate.objectLayers.push_back(std::move(objectLayer));
    std::string validation;
    if (!candidate.validate(&validation)) return fail(error, validation);
    map_ = std::move(candidate);
    selection_ = {};
    selection_.tileValue = map_.tileset.tiles.empty() ? 0U : 1U;
    lastImpact_ = {true, true, true, true, std::nullopt};
    path_.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = true;
    signatureValid_ = false;
    return true;
}

bool TileMapAuthoringSession::open(const std::filesystem::path& path, std::string* error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    TileMap parsed;
    if (!TileMap::parse(text, parsed, error)) return false;
    map_ = std::move(parsed);
    selection_ = {};
    selection_.tileValue = map_.tileset.tiles.empty() ? 0U : 1U;
    lastImpact_ = {};
    path_ = path;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    remember_signature();
    return true;
}

bool TileMapAuthoringSession::save(const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path destination = path.empty() ? path_ : path;
    std::string validation;
    if (!map_.validate(&validation)) return fail(error, validation);
    if (!write_text_recoverable(destination, map_.serialize(), error)) return false;
    path_ = destination;
    dirty_ = false;
    remember_signature();
    return true;
}

bool TileMapAuthoringSession::save_tileset(const std::filesystem::path& path,
                                            std::string* error) const {
    std::string validation;
    if (!map_.tileset.validate(&validation)) return fail(error, validation);
    return write_text_recoverable(path, map_.tileset.serialize(), error);
}

bool TileMapAuthoringSession::update_tileset_tile(std::size_t index, TileDef tile,
                                                        std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.tileset.tiles.size())
            return fail(localError, "embedded tileset tile index is out of range");
        map.tileset.tiles[index] = std::move(tile);
        selection.tileValue = static_cast<std::uint32_t>(index + 1U);
        impact = {true, true, true, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::select_tile_layer(std::size_t index) noexcept {
    if (index >= map_.layers.size()) return false;
    selection_.tileLayer = index;
    selection_.object.reset();
    return true;
}

bool TileMapAuthoringSession::select_tile(std::uint32_t tileValue) noexcept {
    if (tile_id(tileValue) > map_.tileset.tiles.size()) return false;
    selection_.tileValue = tileValue;
    selection_.terrain.clear();
    return true;
}

bool TileMapAuthoringSession::select_terrain(std::string terrain) noexcept {
    if (!terrain.empty()) {
        const bool found = std::any_of(map_.tileset.tiles.begin(), map_.tileset.tiles.end(),
                                       [&](const TileDef& tile) { return tile.terrain == terrain; });
        if (!found) return false;
    }
    selection_.terrain = std::move(terrain);
    return true;
}

bool TileMapAuthoringSession::select_object(std::size_t layerIndex,
                                            std::uint64_t objectId) noexcept {
    if (layerIndex >= map_.objectLayers.size()) return false;
    const auto& objects = map_.objectLayers[layerIndex].objects;
    if (std::none_of(objects.begin(), objects.end(),
                     [&](const TileObject& object) { return object.id == objectId; })) return false;
    selection_.object = std::pair{layerIndex, objectId};
    return true;
}

bool TileMapAuthoringSession::add_tile_layer(std::string name, TileLayerKind kind,
                                             bool collidable, std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string*) {
        const TileLayer& reference = map.layers.front();
        TileLayer layer;
        layer.name = std::move(name);
        layer.width = reference.width;
        layer.height = reference.height;
        layer.tiles.assign(static_cast<std::size_t>(layer.width) * layer.height, 0U);
        layer.kind = kind;
        layer.collidable = collidable;
        map.layers.push_back(std::move(layer));
        selection.tileLayer = map.layers.size() - 1U;
        selection.object.reset();
        impact = {true, true, collidable || kind == TileLayerKind::Collision,
                  kind == TileLayerKind::Hazard || kind == TileLayerKind::Trigger, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::remove_tile_layer(std::size_t index, std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.layers.size()) return fail(localError, "tile-layer index is out of range");
        if (map.layers.size() == 1U) return fail(localError, "a map must retain one tile layer");
        const TileLayer removed = map.layers[index];
        map.layers.erase(map.layers.begin() + static_cast<std::ptrdiff_t>(index));
        if (selection.tileLayer >= map.layers.size()) selection.tileLayer = map.layers.size() - 1U;
        impact = {true, true, layer_affects_collision(removed), layer_affects_semantics(removed),
                  std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::add_object_layer(std::string name, std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string*) {
        TileObjectLayer layer;
        layer.name = std::move(name);
        map.objectLayers.push_back(std::move(layer));
        selection.object.reset();
        impact = {true, true, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::remove_object_layer(std::size_t index, std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.objectLayers.size()) return fail(localError, "object-layer index is out of range");
        map.objectLayers.erase(map.objectLayers.begin() + static_cast<std::ptrdiff_t>(index));
        if (selection.object && selection.object->first == index) selection.object.reset();
        impact = {true, true, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::update_tile_layer(std::size_t index, TileLayer layer,
                                                  std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.layers.size()) return fail(localError, "tile-layer index is out of range");
        const TileLayer previous = map.layers[index];
        if (layer.width != previous.width || layer.height != previous.height ||
            layer.tiles.size() != previous.tiles.size())
            return fail(localError, "layer controls cannot change tile-grid dimensions");
        map.layers[index] = std::move(layer);
        selection.tileLayer = index;
        impact = {true, true,
                  layer_affects_collision(previous) || layer_affects_collision(map.layers[index]),
                  layer_affects_semantics(previous) || layer_affects_semantics(map.layers[index]),
                  std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::move_tile_layer(std::size_t index, std::size_t destination,
                                               std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.layers.size() || destination >= map.layers.size())
            return fail(localError, "tile-layer reorder index is out of range");
        if (index == destination) { impact = {}; return true; }
        TileLayer layer = std::move(map.layers[index]);
        map.layers.erase(map.layers.begin() + static_cast<std::ptrdiff_t>(index));
        map.layers.insert(map.layers.begin() + static_cast<std::ptrdiff_t>(destination),
                          std::move(layer));
        if (selection.tileLayer == index) selection.tileLayer = destination;
        else if (index < selection.tileLayer && selection.tileLayer <= destination) --selection.tileLayer;
        else if (destination <= selection.tileLayer && selection.tileLayer < index) ++selection.tileLayer;
        impact = {true, true, true, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::update_object_layer(std::size_t index, TileObjectLayer layer,
                                                   std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.objectLayers.size())
            return fail(localError, "object-layer index is out of range");
        map.objectLayers[index] = std::move(layer);
        if (selection.object && selection.object->first == index) {
            const std::uint64_t objectId = selection.object->second;
            const auto& objects = map.objectLayers[index].objects;
            if (std::none_of(objects.begin(), objects.end(), [&](const TileObject& object) {
                    return object.id == objectId;
                })) selection.object.reset();
        }
        impact = {true, true, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::move_object_layer(std::size_t index, std::size_t destination,
                                                 std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (index >= map.objectLayers.size() || destination >= map.objectLayers.size())
            return fail(localError, "object-layer reorder index is out of range");
        if (index == destination) { impact = {}; return true; }
        TileObjectLayer layer = std::move(map.objectLayers[index]);
        map.objectLayers.erase(map.objectLayers.begin() + static_cast<std::ptrdiff_t>(index));
        map.objectLayers.insert(map.objectLayers.begin() + static_cast<std::ptrdiff_t>(destination),
                                std::move(layer));
        if (selection.object) {
            if (selection.object->first == index) selection.object->first = destination;
            else if (index < selection.object->first && selection.object->first <= destination)
                --selection.object->first;
            else if (destination <= selection.object->first && selection.object->first < index)
                ++selection.object->first;
        }
        impact = {true, true, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::paint_cell(std::uint32_t col, std::uint32_t row,
                                         std::uint32_t tileValue, std::string* error) {
    return paint_rectangle({col, row, col, row}, tileValue, error);
}

bool TileMapAuthoringSession::paint_rectangle(TileRegion region, std::uint32_t tileValue,
                                              std::string* error) {
    region = normalized_region(region);
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (selection.tileLayer >= map.layers.size()) return fail(localError, "no active tile layer");
        TileLayer& layer = map.layers[selection.tileLayer];
        if (layer.locked) return fail(localError, "active tile layer is locked");
        if (!valid_region(layer, region)) return fail(localError, "paint rectangle is out of range");
        if (tile_id(tileValue) > map.tileset.tiles.size()) return fail(localError, "paint tile id is missing");
        bool changed = false;
        for (std::uint32_t row = region.minRow; row <= region.maxRow; ++row) {
            for (std::uint32_t col = region.minCol; col <= region.maxCol; ++col) {
                if (layer.at(col, row) != tileValue) {
                    layer.set(col, row, tileValue);
                    changed = true;
                }
            }
        }
        impact = {changed, false, changed && layer_affects_collision(layer),
                  changed && layer_affects_semantics(layer), changed ? std::optional{region} : std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::flood_fill(std::uint32_t col, std::uint32_t row,
                                         std::uint32_t tileValue, std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (selection.tileLayer >= map.layers.size()) return fail(localError, "no active tile layer");
        TileLayer& layer = map.layers[selection.tileLayer];
        if (layer.locked) return fail(localError, "active tile layer is locked");
        if (col >= layer.width || row >= layer.height) return fail(localError, "fill seed is out of range");
        if (tile_id(tileValue) > map.tileset.tiles.size()) return fail(localError, "fill tile id is missing");
        const std::uint32_t source = layer.at(col, row);
        if (source == tileValue) { impact = {}; return true; }
        std::vector<std::uint8_t> visited(layer.tiles.size(), 0U);
        std::queue<std::pair<std::uint32_t, std::uint32_t>> pending;
        pending.push({col, row});
        TileRegion dirty{col, row, col, row};
        while (!pending.empty()) {
            const auto [currentCol, currentRow] = pending.front();
            pending.pop();
            const std::size_t offset = static_cast<std::size_t>(currentRow) * layer.width + currentCol;
            if (visited[offset] != 0U || layer.at(currentCol, currentRow) != source) continue;
            visited[offset] = 1U;
            layer.set(currentCol, currentRow, tileValue);
            dirty.minCol = std::min(dirty.minCol, currentCol);
            dirty.minRow = std::min(dirty.minRow, currentRow);
            dirty.maxCol = std::max(dirty.maxCol, currentCol);
            dirty.maxRow = std::max(dirty.maxRow, currentRow);
            if (currentCol > 0U) pending.push({currentCol - 1U, currentRow});
            if (currentCol + 1U < layer.width) pending.push({currentCol + 1U, currentRow});
            if (currentRow > 0U) pending.push({currentCol, currentRow - 1U});
            if (currentRow + 1U < layer.height) pending.push({currentCol, currentRow + 1U});
        }
        impact = {true, false, layer_affects_collision(layer), layer_affects_semantics(layer), dirty};
        return true;
    }, error);
}

bool TileMapAuthoringSession::paint_terrain(std::uint32_t col, std::uint32_t row,
                                            std::string terrain, bool erase,
                                            std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (selection.tileLayer >= map.layers.size()) return fail(localError, "no active tile layer");
        TileLayer& layer = map.layers[selection.tileLayer];
        if (layer.locked) return fail(localError, "active tile layer is locked");
        if (col >= layer.width || row >= layer.height) return fail(localError, "terrain cell is out of range");
        const std::uint32_t value = erase ? 0U : map.tileset.autotile_value(terrain, 0U);
        if (!erase && value == 0U) return fail(localError, "terrain has no tile or autotile rule");
        if (layer.at(col, row) == value && erase) { impact = {}; return true; }
        layer.set(col, row, value);
        refresh_autotile_in_map(map, selection.tileLayer, {col, row, col, row}, terrain);
        const TileRegion dirty{col > 0U ? col - 1U : 0U, row > 0U ? row - 1U : 0U,
            std::min(layer.width - 1U, col + 1U), std::min(layer.height - 1U, row + 1U)};
        selection.terrain = std::move(terrain);
        impact = {true, false, layer_affects_collision(layer), layer_affects_semantics(layer), dirty};
        return true;
    }, error);
}

bool TileMapAuthoringSession::refresh_autotile(TileRegion region, std::string terrain,
                                               std::string* error) {
    region = normalized_region(region);
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (selection.tileLayer >= map.layers.size()) return fail(localError, "no active tile layer");
        TileLayer& layer = map.layers[selection.tileLayer];
        if (layer.locked) return fail(localError, "active tile layer is locked");
        if (!valid_region(layer, region)) return fail(localError, "autotile region is out of range");
        const std::vector<std::uint32_t> before = layer.tiles;
        refresh_autotile_in_map(map, selection.tileLayer, region, terrain);
        const bool changed = before != layer.tiles;
        impact = {changed, false, changed && layer_affects_collision(layer),
                  changed && layer_affects_semantics(layer), changed ? std::optional{region} : std::nullopt};
        return true;
    }, error);
}

std::uint64_t TileMapAuthoringSession::next_object_id() const noexcept {
    std::uint64_t maximum = 0U;
    for (const TileObjectLayer& layer : map_.objectLayers)
        for (const TileObject& object : layer.objects) maximum = std::max(maximum, object.id);
    return maximum == std::numeric_limits<std::uint64_t>::max() ? 0U : maximum + 1U;
}

bool TileMapAuthoringSession::add_object(std::size_t layerIndex, TileObject object,
                                         std::uint64_t* assignedId, std::string* error) {
    std::uint64_t id = object.id;
    if (id == 0U) id = next_object_id();
    if (id == 0U) return fail(error, "object id space is exhausted");
    object.id = id;
    const bool result = apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                                       TileEditImpact& impact, std::string* localError) {
        if (layerIndex >= map.objectLayers.size()) return fail(localError, "object-layer index is out of range");
        if (map.objectLayers[layerIndex].locked) return fail(localError, "object layer is locked");
        for (const TileObjectLayer& layer : map.objectLayers)
            if (std::any_of(layer.objects.begin(), layer.objects.end(),
                            [&](const TileObject& existing) { return existing.id == object.id; }))
                return fail(localError, "object id already exists");
        map.objectLayers[layerIndex].objects.push_back(object);
        selection.object = std::pair{layerIndex, object.id};
        impact = {true, false, false, true, std::nullopt};
        return true;
    }, error);
    if (result && assignedId != nullptr) *assignedId = id;
    return result;
}

bool TileMapAuthoringSession::update_object(std::size_t layerIndex, TileObject object,
                                            std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (layerIndex >= map.objectLayers.size()) return fail(localError, "object-layer index is out of range");
        if (map.objectLayers[layerIndex].locked) return fail(localError, "object layer is locked");
        auto& objects = map.objectLayers[layerIndex].objects;
        const auto found = std::find_if(objects.begin(), objects.end(),
                                        [&](const TileObject& current) { return current.id == object.id; });
        if (found == objects.end()) return fail(localError, "object id was not found");
        *found = object;
        selection.object = std::pair{layerIndex, object.id};
        impact = {true, false, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::remove_object(std::size_t layerIndex, std::uint64_t objectId,
                                            std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (layerIndex >= map.objectLayers.size()) return fail(localError, "object-layer index is out of range");
        if (map.objectLayers[layerIndex].locked) return fail(localError, "object layer is locked");
        auto& objects = map.objectLayers[layerIndex].objects;
        const auto found = std::find_if(objects.begin(), objects.end(),
                                        [&](const TileObject& object) { return object.id == objectId; });
        if (found == objects.end()) return fail(localError, "object id was not found");
        objects.erase(found);
        if (selection.object == std::optional{std::pair{layerIndex, objectId}}) selection.object.reset();
        impact = {true, false, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::set_object_property(std::size_t layerIndex,
                                                  std::uint64_t objectId,
                                                  std::string name, std::string value,
                                                  std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (layerIndex >= map.objectLayers.size()) return fail(localError, "object-layer index is out of range");
        auto& objects = map.objectLayers[layerIndex].objects;
        const auto found = std::find_if(objects.begin(), objects.end(),
                                        [&](const TileObject& object) { return object.id == objectId; });
        if (found == objects.end()) return fail(localError, "object id was not found");
        auto property = std::find_if(found->properties.begin(), found->properties.end(),
                                     [&](const TileObjectProperty& item) { return item.name == name; });
        if (property == found->properties.end()) found->properties.push_back({std::move(name), std::move(value)});
        else property->value = std::move(value);
        selection.object = std::pair{layerIndex, objectId};
        impact = {true, false, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::update_object_shape(
    std::size_t layerIndex, std::uint64_t objectId, TileObjectShape shape,
    TileVec2 position, TileVec2 size, float rotationDegrees, std::string* error) {
    return apply_edit([&](TileMap& map, TileMapAuthoringSelection& selection,
                          TileEditImpact& impact, std::string* localError) {
        if (layerIndex >= map.objectLayers.size())
            return fail(localError, "object-layer index is out of range");
        if (map.objectLayers[layerIndex].locked)
            return fail(localError, "object layer is locked");
        auto& objects = map.objectLayers[layerIndex].objects;
        const auto found = std::find_if(objects.begin(), objects.end(),
                                        [&](const TileObject& object) { return object.id == objectId; });
        if (found == objects.end()) return fail(localError, "object id was not found");
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(size.x) || !std::isfinite(size.y) ||
            !std::isfinite(rotationDegrees))
            return fail(localError, "collision shape contains a non-finite value");
        if (shape != TileObjectShape::Point && (size.x <= 0.0F || size.y <= 0.0F))
            return fail(localError, "collision shape size must be positive");
        found->shape = shape;
        found->position = position;
        found->size = shape == TileObjectShape::Point ? TileVec2{} : size;
        found->rotationDegrees = rotationDegrees;
        selection.object = std::pair{layerIndex, objectId};
        impact = {true, false, false, true, std::nullopt};
        return true;
    }, error);
}

bool TileMapAuthoringSession::undo(std::string* error) {
    if (undo_.empty()) return fail(error, "tile-map undo history is empty");
    if (redo_.size() == kMaxUndo) redo_.erase(redo_.begin());
    redo_.push_back({map_, selection_});
    Snapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    map_ = std::move(snapshot.map);
    selection_ = std::move(snapshot.selection);
    normalize_selection();
    dirty_ = true;
    lastImpact_ = {true, true, true, true, std::nullopt};
    return true;
}

bool TileMapAuthoringSession::redo(std::string* error) {
    if (redo_.empty()) return fail(error, "tile-map redo history is empty");
    if (undo_.size() == kMaxUndo) undo_.erase(undo_.begin());
    undo_.push_back({map_, selection_});
    Snapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    map_ = std::move(snapshot.map);
    selection_ = std::move(snapshot.selection);
    normalize_selection();
    dirty_ = true;
    lastImpact_ = {true, true, true, true, std::nullopt};
    return true;
}

TileMapExternalChangeResult TileMapAuthoringSession::poll_external_change(bool force) {
    TileMapExternalChangeResult result;
    if (path_.empty()) return result;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        result.state = TileMapExternalChange::Missing;
        result.message = "Tile-map file is missing or unreadable";
        return result;
    }
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) {
        result.state = TileMapExternalChange::Missing;
        result.message = "Tile-map file size is unavailable";
        return result;
    }
    if (!force && signatureValid_ && time == writeTime_ && size == fileSize_) return result;
    if (dirty_) {
        result.state = TileMapExternalChange::Conflict;
        result.message = "Tile map changed on disk while local edits are unsaved";
        return result;
    }
    std::string text;
    std::string error;
    TileMap parsed;
    if (!read_text(path_, text, &error) || !TileMap::parse(text, parsed, &error)) {
        result.state = TileMapExternalChange::Failed;
        result.message = error;
        return result;
    }
    map_ = std::move(parsed);
    selection_ = {};
    selection_.tileValue = map_.tileset.tiles.empty() ? 0U : 1U;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    lastImpact_ = {true, true, true, true, std::nullopt};
    writeTime_ = time;
    fileSize_ = size;
    signatureValid_ = true;
    result.state = TileMapExternalChange::Reloaded;
    result.message = "Reloaded changed tile-map asset";
    return result;
}

void TileMapAuthoringSession::normalize_selection() noexcept {
    if (map_.layers.empty()) selection_.tileLayer = 0U;
    else if (selection_.tileLayer >= map_.layers.size()) selection_.tileLayer = map_.layers.size() - 1U;
    if (tile_id(selection_.tileValue) > map_.tileset.tiles.size()) selection_.tileValue = 0U;
    if (selection_.object) {
        const auto [layerIndex, objectId] = *selection_.object;
        if (layerIndex >= map_.objectLayers.size() ||
            std::none_of(map_.objectLayers[layerIndex].objects.begin(),
                         map_.objectLayers[layerIndex].objects.end(),
                         [&](const TileObject& object) { return object.id == objectId; }))
            selection_.object.reset();
    }
}

void TileMapAuthoringSession::remember_signature() noexcept {
    signatureValid_ = false;
    if (path_.empty()) return;
    std::error_code ec;
    writeTime_ = std::filesystem::last_write_time(path_, ec);
    if (ec) return;
    fileSize_ = std::filesystem::file_size(path_, ec);
    signatureValid_ = !ec;
}

bool TileMapCanvasWorkspace::set_zoom(float zoom) noexcept {
    if (!std::isfinite(zoom)) return false;
    zoom_ = std::clamp(zoom, 0.125F, 16.0F);
    return true;
}

TileVec2 TileMapCanvasWorkspace::screen_to_world(TileVec2 point,
                                                 TileCanvasRect viewport) const noexcept {
    const float inverse = zoom_ > 0.0001F ? 1.0F / zoom_ : 1.0F;
    return {(point.x - viewport.x - pan_.x) * inverse,
            (point.y - viewport.y - pan_.y) * inverse};
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
TileMapCanvasWorkspace::screen_to_cell(TileVec2 point, TileCanvasRect viewport) const noexcept {
    if (!viewport.contains(point) || session_.map().layers.empty()) return std::nullopt;
    const TileVec2 world = screen_to_world(point, viewport);
    if (world.x < 0.0F || world.y < 0.0F) return std::nullopt;
    const std::uint32_t col = static_cast<std::uint32_t>(std::floor(world.x / static_cast<float>(session_.map().tileWidth)));
    const std::uint32_t row = static_cast<std::uint32_t>(std::floor(world.y / static_cast<float>(session_.map().tileHeight)));
    const TileLayer& layer = session_.map().layers[session_.selection().tileLayer];
    if (col >= layer.width || row >= layer.height) return std::nullopt;
    return std::pair{col, row};
}

std::optional<std::pair<std::size_t, std::uint64_t>> TileMapCanvasWorkspace::object_at(
    TileVec2 point, TileCanvasRect viewport) const noexcept {
    if (!viewport.contains(point)) return std::nullopt;
    const TileVec2 world = screen_to_world(point, viewport);
    for (std::size_t layerIndex = session_.map().objectLayers.size(); layerIndex-- > 0U;) {
        const TileObjectLayer& layer = session_.map().objectLayers[layerIndex];
        if (!layer.visible) continue;
        for (std::size_t i = layer.objects.size(); i-- > 0U;)
            if (object_overlap(layer.objects[i], world)) return std::pair{layerIndex, layer.objects[i].id};
    }
    return std::nullopt;
}

TileMapCanvasFrame TileMapCanvasWorkspace::frame(TileCanvasRect viewport,
                                                 std::uint64_t animationTicks) const {
    TileMapCanvasFrame result;
    result.viewport = viewport;
    result.zoom = zoom_;
    result.pan = pan_;
    const TileMap& map = session_.map();
    if (map.layers.empty() || map.tileWidth == 0U || map.tileHeight == 0U) return result;
    const TileVec2 worldMin = screen_to_world({viewport.x, viewport.y}, viewport);
    const TileVec2 worldMax = screen_to_world({viewport.x + viewport.width,
                                                viewport.y + viewport.height}, viewport);
    const std::int64_t minCol = static_cast<std::int64_t>(std::floor(worldMin.x / static_cast<float>(map.tileWidth)));
    const std::int64_t minRow = static_cast<std::int64_t>(std::floor(worldMin.y / static_cast<float>(map.tileHeight)));
    const std::int64_t maxCol = static_cast<std::int64_t>(std::ceil(worldMax.x / static_cast<float>(map.tileWidth)));
    const std::int64_t maxRow = static_cast<std::int64_t>(std::ceil(worldMax.y / static_cast<float>(map.tileHeight)));
    const TileMapAuthoringSelection selection = session_.selection();
    for (std::size_t layerIndex = 0U; layerIndex < map.layers.size(); ++layerIndex) {
        const TileLayer& layer = map.layers[layerIndex];
        if (!layer.visible) continue;
        for (std::int64_t row = std::max<std::int64_t>(0, minRow);
             row < maxRow && row < static_cast<std::int64_t>(layer.height); ++row) {
            for (std::int64_t col = std::max<std::int64_t>(0, minCol);
                 col < maxCol && col < static_cast<std::int64_t>(layer.width); ++col) {
                if (result.cells.size() >= kMaxCanvasCells) break;
                const std::uint32_t value = layer.at(static_cast<std::uint32_t>(col),
                                                     static_cast<std::uint32_t>(row));
                if (value == 0U && layerIndex != selection.tileLayer) continue;
                const float x = viewport.x + pan_.x + static_cast<float>(col * map.tileWidth) * zoom_;
                const float y = viewport.y + pan_.y + static_cast<float>(row * map.tileHeight) * zoom_;
                result.cells.push_back(TileCanvasCellFrame{
                    layerIndex, static_cast<std::uint32_t>(col), static_cast<std::uint32_t>(row), value,
                    map.tileset.atlas_index_for(value, animationTicks),
                    {x, y, static_cast<float>(map.tileWidth) * zoom_,
                     static_cast<float>(map.tileHeight) * zoom_},
                    layer.kind, map.tileset.collision_for(value), layerIndex == selection.tileLayer});
            }
        }
    }
    for (std::size_t layerIndex = 0U; layerIndex < map.objectLayers.size(); ++layerIndex) {
        const TileObjectLayer& layer = map.objectLayers[layerIndex];
        if (!layer.visible) continue;
        for (const TileObject& object : layer.objects) {
            const float width = object.shape == TileObjectShape::Point ? 10.0F : object.size.x * zoom_;
            const float height = object.shape == TileObjectShape::Point ? 10.0F : object.size.y * zoom_;
            const float x = viewport.x + pan_.x + object.position.x * zoom_ -
                (object.shape == TileObjectShape::Point ? 5.0F : 0.0F);
            const float y = viewport.y + pan_.y + object.position.y * zoom_ -
                (object.shape == TileObjectShape::Point ? 5.0F : 0.0F);
            if (x + width < viewport.x || y + height < viewport.y ||
                x > viewport.x + viewport.width || y > viewport.y + viewport.height) continue;
            const bool selected = selection.object == std::optional{std::pair{layerIndex, object.id}};
            std::array<TileCanvasRect, 4> handles{};
            if (selected && object.shape != TileObjectShape::Point) {
                constexpr float handleSize = 8.0F;
                handles[0] = {x - handleSize * 0.5F, y + height * 0.5F - handleSize * 0.5F,
                              handleSize, handleSize};
                handles[1] = {x + width - handleSize * 0.5F,
                              y + height * 0.5F - handleSize * 0.5F,
                              handleSize, handleSize};
                handles[2] = {x + width * 0.5F - handleSize * 0.5F, y - handleSize * 0.5F,
                              handleSize, handleSize};
                handles[3] = {x + width * 0.5F - handleSize * 0.5F,
                              y + height - handleSize * 0.5F,
                              handleSize, handleSize};
            }
            result.objects.push_back(TileCanvasObjectFrame{
                layerIndex, object.id, {x, y, width, height}, object.shape, selected, handles});
        }
    }
    if (capture_ == Capture::Rectangle && startCell_ && currentCell_) {
        result.brushPreview = normalized_region({startCell_->first, startCell_->second,
                                                  currentCell_->first, currentCell_->second});
    }
    return result;
}

bool TileMapCanvasWorkspace::pointer_down(int button, TileVec2 point,
                                          TileCanvasRect viewport, std::string* error) {
    pointerDown_ = point;
    if (button == 2 || button == 3 || tool_ == TileMapTool::Pan) {
        capture_ = Capture::Pan;
        capturePan_ = pan_;
        return true;
    }
    if (button != 1 || !viewport.contains(point)) return false;
    if (tool_ == TileMapTool::Object || tool_ == TileMapTool::CollisionShape) {
        const TileMapCanvasFrame currentFrame = frame(viewport);
        for (const TileCanvasObjectFrame& objectFrame : currentFrame.objects) {
            if (!objectFrame.selected) continue;
            for (std::size_t handle = 0U; handle < objectFrame.resizeHandles.size(); ++handle) {
                if (!objectFrame.resizeHandles[handle].contains(point)) continue;
                movingObject_ = std::pair{objectFrame.layerIndex, objectFrame.objectId};
                const auto& objects = session_.map().objectLayers[objectFrame.layerIndex].objects;
                const auto found = std::find_if(objects.begin(), objects.end(), [&](const TileObject& object) {
                    return object.id == objectFrame.objectId;
                });
                if (found == objects.end()) return fail(error, "selected collision object disappeared");
                movingObjectStart_ = *found;
                resizeHandle_ = static_cast<ResizeHandle>(handle + 1U);
                capture_ = Capture::ObjectResize;
                return true;
            }
        }
        movingObject_ = object_at(point, viewport);
        if (movingObject_) {
            const auto [layerIndex, objectId] = *movingObject_;
            if (!session_.select_object(layerIndex, objectId))
                return fail(error, "could not select tile object");
            const auto& objects = session_.map().objectLayers[layerIndex].objects;
            const auto found = std::find_if(objects.begin(), objects.end(),
                                            [&](const TileObject& object) { return object.id == objectId; });
            movingObjectStart_ = *found;
            capture_ = Capture::ObjectMove;
            return true;
        }
        if (session_.map().objectLayers.empty()) return fail(error, "map has no object layer");
        const TileVec2 world = screen_to_world(point, viewport);
        TileObject object;
        object.name = "Object";
        object.type = "object";
        object.position = world;
        return session_.add_object(0U, std::move(object), nullptr, error);
    }
    const auto cell = screen_to_cell(point, viewport);
    if (!cell) return false;
    startCell_ = cell;
    currentCell_ = cell;
    const TileMapAuthoringSelection selection = session_.selection();
    if (tool_ == TileMapTool::Rectangle) {
        capture_ = Capture::Rectangle;
        return true;
    }
    if (tool_ == TileMapTool::FloodFill) {
        return session_.flood_fill(cell->first, cell->second, selection.tileValue, error);
    }
    if (tool_ == TileMapTool::Terrain) {
        return session_.paint_terrain(cell->first, cell->second, selection.terrain, false, error);
    }
    capture_ = Capture::Paint;
    const std::uint32_t value = tool_ == TileMapTool::Eraser ? 0U : selection.tileValue;
    return session_.paint_cell(cell->first, cell->second, value, error);
}

bool TileMapCanvasWorkspace::pointer_move(TileVec2 point, TileCanvasRect viewport,
                                          std::string* error) {
    if (capture_ == Capture::Pan) {
        pan_ = {capturePan_.x + point.x - pointerDown_.x,
                capturePan_.y + point.y - pointerDown_.y};
        return true;
    }
    if (capture_ == Capture::ObjectMove || capture_ == Capture::ObjectResize) return true;
    const auto cell = screen_to_cell(point, viewport);
    if (!cell) return false;
    if (cell == currentCell_) return true;
    const auto previous = currentCell_;
    currentCell_ = cell;
    if (capture_ != Capture::Paint) return true;
    const TileMapAuthoringSelection selection = session_.selection();
    if (tool_ == TileMapTool::Terrain)
        return session_.paint_terrain(cell->first, cell->second, selection.terrain, false, error);
    const std::uint32_t value = tool_ == TileMapTool::Eraser ? 0U : selection.tileValue;
    if (previous) {
        return session_.paint_rectangle({std::min(previous->first, cell->first),
                                         std::min(previous->second, cell->second),
                                         std::max(previous->first, cell->first),
                                         std::max(previous->second, cell->second)}, value, error);
    }
    return session_.paint_cell(cell->first, cell->second, value, error);
}

bool TileMapCanvasWorkspace::pointer_up(int button, TileVec2 point,
                                        TileCanvasRect viewport, std::string* error) {
    if (button != 1 && capture_ != Capture::Pan) return false;
    bool result = true;
    if (capture_ == Capture::Rectangle && startCell_) {
        const auto cell = screen_to_cell(point, viewport);
        if (cell) {
            const TileMapAuthoringSelection selection = session_.selection();
            result = session_.paint_rectangle({startCell_->first, startCell_->second,
                                                cell->first, cell->second},
                                               selection.tileValue, error);
        }
    } else if (capture_ == Capture::ObjectMove && movingObject_) {
        TileObject object = movingObjectStart_;
        const TileVec2 startWorld = screen_to_world(pointerDown_, viewport);
        const TileVec2 currentWorld = screen_to_world(point, viewport);
        object.position.x += currentWorld.x - startWorld.x;
        object.position.y += currentWorld.y - startWorld.y;
        result = session_.update_object(movingObject_->first, std::move(object), error);
    } else if (capture_ == Capture::ObjectResize && movingObject_) {
        TileObject object = movingObjectStart_;
        if (object.shape == TileObjectShape::Point) object.shape = TileObjectShape::Rectangle;
        const TileVec2 currentWorld = screen_to_world(point, viewport);
        const float originalRight = object.position.x + object.size.x;
        const float originalBottom = object.position.y + object.size.y;
        switch (resizeHandle_) {
            case ResizeHandle::Left:
                object.position.x = std::min(currentWorld.x, originalRight - 1.0F);
                object.size.x = originalRight - object.position.x;
                break;
            case ResizeHandle::Right:
                object.size.x = std::max(1.0F, currentWorld.x - object.position.x);
                break;
            case ResizeHandle::Top:
                object.position.y = std::min(currentWorld.y, originalBottom - 1.0F);
                object.size.y = originalBottom - object.position.y;
                break;
            case ResizeHandle::Bottom:
                object.size.y = std::max(1.0F, currentWorld.y - object.position.y);
                break;
            case ResizeHandle::NoHandle: break;
        }
        result = session_.update_object_shape(movingObject_->first, object.id, object.shape,
                                              object.position, object.size,
                                              object.rotationDegrees, error);
    }
    capture_ = Capture::NoCapture;
    startCell_.reset();
    currentCell_.reset();
    movingObject_.reset();
    resizeHandle_ = ResizeHandle::NoHandle;
    return result;
}

bool TileMapCanvasWorkspace::wheel(float steps, TileVec2 point,
                                    TileCanvasRect viewport) noexcept {
    if (!std::isfinite(steps) || !viewport.contains(point)) return false;
    const TileVec2 worldBefore = screen_to_world(point, viewport);
    const float factor = std::pow(1.125F, steps);
    if (!set_zoom(zoom_ * factor)) return false;
    pan_.x = point.x - viewport.x - worldBefore.x * zoom_;
    pan_.y = point.y - viewport.y - worldBefore.y * zoom_;
    return true;
}


bool TileWorldDesktopWorkspace::open(const std::filesystem::path& mapPath,
                                     const std::filesystem::path& tilesetPath,
                                     std::string* error) {
    if (!canvas_.session().open(mapPath, error)) return false;
    std::filesystem::path resolvedTileset = tilesetPath;
    if (resolvedTileset.empty() && !canvas_.session().map().tilesetAsset.empty()) {
        const std::filesystem::path reference = canvas_.session().map().tilesetAsset;
        const std::filesystem::path sibling = mapPath.parent_path() / reference.filename();
        const std::filesystem::path rooted = mapPath.parent_path().parent_path() / reference;
        if (std::filesystem::exists(sibling)) resolvedTileset = sibling;
        else if (std::filesystem::exists(rooted)) resolvedTileset = rooted;
    }
    if (!resolvedTileset.empty()) {
        std::string paletteError;
        if (!palette_.session().open(resolvedTileset, &paletteError))
            return fail(error, paletteError);
    }
    previewCamera_ = {};
    previewCamera_.viewWidth = 320.0F;
    previewCamera_.viewHeight = 180.0F;
    camera_set_world_from_map(previewCamera_, canvas_.session().map());
    previewCamera_.center = {previewCamera_.viewWidth * 0.5F,
                             previewCamera_.viewHeight * 0.5F};
    playRequested_ = false;
    recookGeneration_ = 0U;
    recookOverlay_.reset();
    inspectorKind_ = TileWorldInspectorKind::Tile;
    return true;
}

bool TileWorldDesktopWorkspace::save(std::string* error) {
    if (!canvas_.session().save({}, error)) return false;
    if (palette_.session().dirty() && !palette_.session().path().empty() &&
        !palette_.session().save({}, error)) return false;
    return true;
}

bool TileWorldDesktopWorkspace::select_tile_layer(std::size_t index) noexcept {
    const bool selected = canvas_.session().select_tile_layer(index);
    if (selected) inspectorKind_ = TileWorldInspectorKind::Tile;
    return selected;
}

bool TileWorldDesktopWorkspace::canvas_pointer_down(int button, TileVec2 point,
                                                       TileCanvasRect viewport, std::string* error) {
    const bool changed = canvas_.pointer_down(button, point, viewport, error);
    if (changed && canvas_.session().last_impact().changed) record_recook_overlay();
    return changed;
}

bool TileWorldDesktopWorkspace::canvas_pointer_move(TileVec2 point, TileCanvasRect viewport,
                                                      std::string* error) {
    const bool changed = canvas_.pointer_move(point, viewport, error);
    if (changed && canvas_.session().last_impact().changed) record_recook_overlay();
    return changed;
}

bool TileWorldDesktopWorkspace::canvas_pointer_up(int button, TileVec2 point,
                                                    TileCanvasRect viewport, std::string* error) {
    const bool changed = canvas_.pointer_up(button, point, viewport, error);
    if (changed && canvas_.session().last_impact().changed) record_recook_overlay();
    return changed;
}

bool TileWorldDesktopWorkspace::canvas_wheel(float steps, TileVec2 point,
                                               TileCanvasRect viewport) noexcept {
    return canvas_.wheel(steps, point, viewport);
}

bool TileWorldDesktopWorkspace::palette_pointer_down(int button, TileVec2 point,
                                                       TileCanvasRect viewport) noexcept {
    const bool changed = palette_.pointer_down(button, point, viewport);
    if (changed && button == 1) {
        const std::size_t selected = palette_.session().selected_tile();
        (void)canvas_.session().select_tile(static_cast<std::uint32_t>(selected + 1U));
        inspectorKind_ = TileWorldInspectorKind::Tile;
    }
    return changed;
}

bool TileWorldDesktopWorkspace::palette_pointer_move(TileVec2 point) noexcept {
    return palette_.pointer_move(point);
}

bool TileWorldDesktopWorkspace::palette_pointer_up(int button) noexcept {
    return palette_.pointer_up(button);
}

bool TileWorldDesktopWorkspace::palette_wheel(float steps, TileVec2 point,
                                                TileCanvasRect viewport) noexcept {
    return palette_.wheel(steps, point, viewport);
}

bool TileWorldDesktopWorkspace::select_object(std::size_t layerIndex,
                                               std::uint64_t objectId) noexcept {
    if (!canvas_.session().select_object(layerIndex, objectId)) return false;
    const TileObjectLayer& layer = canvas_.session().map().objectLayers[layerIndex];
    const auto found = std::find_if(layer.objects.begin(), layer.objects.end(),
        [&](const TileObject& object) { return object.id == objectId; });
    if (found == layer.objects.end()) return false;
    if (found->type == "spawn") inspectorKind_ = TileWorldInspectorKind::Spawn;
    else if (found->type == "trigger") inspectorKind_ = TileWorldInspectorKind::Trigger;
    else if (found->type == "hazard") inspectorKind_ = TileWorldInspectorKind::Hazard;
    else inspectorKind_ = TileWorldInspectorKind::Object;
    return true;
}

bool TileWorldDesktopWorkspace::update_selected_tile(TileDef tile, std::string* error) {
    if (palette_.session().tileset().tiles.empty())
        return fail(error, "no reusable tileset is open");
    const std::size_t index = palette_.session().selected_tile();
    if (!palette_.session().update_tile(index, tile, error)) return false;
    if (!canvas_.session().update_tileset_tile(index, std::move(tile), error)) return false;
    record_recook_overlay();
    return true;
}

bool TileWorldDesktopWorkspace::update_selected_tile_layer(TileLayer layer,
                                                            std::string* error) {
    const std::size_t index = canvas_.session().selection().tileLayer;
    if (!canvas_.session().update_tile_layer(index, std::move(layer), error)) return false;
    record_recook_overlay();
    return true;
}

bool TileWorldDesktopWorkspace::update_selected_object(TileObject object,
                                                        std::string* error) {
    const auto selected = canvas_.session().selection().object;
    if (!selected) return fail(error, "no tile-world object is selected");
    if (object.id == 0U) object.id = selected->second;
    if (!canvas_.session().update_object(selected->first, std::move(object), error)) return false;
    record_recook_overlay();
    return true;
}

bool TileWorldDesktopWorkspace::update_selected_object_layer(TileObjectLayer layer,
                                                              std::string* error) {
    const auto selected = canvas_.session().selection().object;
    if (!selected) return fail(error, "no object layer is selected");
    if (!canvas_.session().update_object_layer(selected->first, std::move(layer), error)) return false;
    record_recook_overlay();
    return true;
}

bool TileWorldDesktopWorkspace::resize_selected_collision_shape(
    TileVec2 minimum, TileVec2 size, float rotationDegrees, std::string* error) {
    const auto selected = canvas_.session().selection().object;
    if (!selected) return fail(error, "no collision object is selected");
    const TileObjectLayer& layer = canvas_.session().map().objectLayers[selected->first];
    const auto found = std::find_if(layer.objects.begin(), layer.objects.end(),
        [&](const TileObject& object) { return object.id == selected->second; });
    if (found == layer.objects.end()) return fail(error, "selected collision object is missing");
    const TileObjectShape shape = found->shape == TileObjectShape::Point
        ? TileObjectShape::Rectangle : found->shape;
    if (!canvas_.session().update_object_shape(selected->first, selected->second, shape,
                                                minimum, size, rotationDegrees, error)) return false;
    record_recook_overlay();
    return true;
}

bool TileWorldDesktopWorkspace::place_prefab(
    std::size_t objectLayer, std::string prefabAsset, TileVec2 position,
    std::vector<TileObjectProperty> overrides, std::uint64_t* assignedId,
    std::string* error) {
    if (prefabAsset.empty()) return fail(error, "prefab asset reference is empty");
    TileObject object;
    object.name = std::filesystem::path(prefabAsset).stem().string();
    object.type = "prefab";
    object.shape = TileObjectShape::Point;
    object.position = position;
    object.properties.push_back({"prefab_asset", std::move(prefabAsset)});
    for (TileObjectProperty& property : overrides) {
        if (property.name.empty()) return fail(error, "prefab override name is empty");
        property.name = "override." + property.name;
        object.properties.push_back(std::move(property));
    }
    if (!canvas_.session().add_object(objectLayer, std::move(object), assignedId, error)) return false;
    inspectorKind_ = TileWorldInspectorKind::Object;
    record_recook_overlay();
    return true;
}

bool TileWorldDesktopWorkspace::set_prefab_override(std::string name, std::string value,
                                                     std::string* error) {
    const auto selected = canvas_.session().selection().object;
    if (!selected) return fail(error, "no prefab object is selected");
    const TileObjectLayer& layer = canvas_.session().map().objectLayers[selected->first];
    const auto found = std::find_if(layer.objects.begin(), layer.objects.end(),
        [&](const TileObject& object) { return object.id == selected->second; });
    if (found == layer.objects.end() || found->type != "prefab")
        return fail(error, "selected object is not a prefab placement");
    if (!name.starts_with("override.")) name = "override." + name;
    if (!canvas_.session().set_object_property(selected->first, selected->second,
                                                std::move(name), std::move(value), error)) return false;
    record_recook_overlay();
    return true;
}

std::vector<TileWorldDiagnostic> TileWorldDesktopWorkspace::validate_level() const {
    std::vector<TileWorldDiagnostic> result;
    const TileMap& map = canvas_.session().map();
    std::string validation;
    if (!map.validate(&validation))
        result.push_back({TileWorldDiagnosticSeverity::Error, "map", validation});
    if (map.layers.empty())
        result.push_back({TileWorldDiagnosticSeverity::Error, "layers", "Level has no tile layers"});
    if (map.objectLayers.empty())
        result.push_back({TileWorldDiagnosticSeverity::Warning, "object layers",
                          "Level has no object, spawn, trigger, or prefab layer"});

    std::set<std::string, std::less<>> layerNames;
    bool hasSpawn = false;
    bool hasCheckpoint = false;
    bool hasVisual = false;
    for (std::size_t layerIndex = 0U; layerIndex < map.layers.size(); ++layerIndex) {
        const TileLayer& layer = map.layers[layerIndex];
        const std::string location = "tile layer " + std::to_string(layerIndex) + " (" + layer.name + ")";
        if (!layerNames.insert(layer.name).second)
            result.push_back({TileWorldDiagnosticSeverity::Warning, location,
                              "Layer name is duplicated"});
        if (layer.kind == TileLayerKind::Visual && layer.visible) hasVisual = true;
        for (std::uint32_t row = 0U; row < layer.height; ++row) {
            for (std::uint32_t col = 0U; col < layer.width; ++col) {
                const std::uint32_t id = tile_id(layer.at(col, row));
                if (id == 0U) continue;
                const std::string cell = location + " cell " + std::to_string(col) + "," +
                    std::to_string(row);
                if (id > map.tileset.tiles.size()) {
                    result.push_back({TileWorldDiagnosticSeverity::Error, cell,
                                      "Tile id is missing from the embedded tileset"});
                    continue;
                }
                const TileDef& tile = map.tileset.tiles[id - 1U];
                if (layer.kind == TileLayerKind::Hazard && tile.damagePerSecond <= 0.0F &&
                    tile.trigger.empty())
                    result.push_back({TileWorldDiagnosticSeverity::Warning, cell,
                                      "Hazard tile has no damage or trigger behavior"});
                if (layer.kind == TileLayerKind::Trigger && tile.trigger.empty())
                    result.push_back({TileWorldDiagnosticSeverity::Warning, cell,
                                      "Trigger-layer tile has no trigger identifier"});
            }
        }
    }
    if (!hasVisual)
        result.push_back({TileWorldDiagnosticSeverity::Warning, "layers",
                          "No visible visual tile layer is enabled"});

    std::set<std::uint64_t> objectIds;
    for (std::size_t layerIndex = 0U; layerIndex < map.objectLayers.size(); ++layerIndex) {
        const TileObjectLayer& layer = map.objectLayers[layerIndex];
        for (const TileObject& object : layer.objects) {
            const std::string location = "object layer " + std::to_string(layerIndex) +
                " object " + std::to_string(object.id) + " (" + object.name + ")";
            if (!objectIds.insert(object.id).second)
                result.push_back({TileWorldDiagnosticSeverity::Error, location,
                                  "Object id is duplicated across layers"});
            if (object.type.empty())
                result.push_back({TileWorldDiagnosticSeverity::Warning, location,
                                  "Object type is empty"});
            if (object.type == "spawn") hasSpawn = true;
            if (object.type == "checkpoint") hasCheckpoint = true;
            if ((object.shape == TileObjectShape::Rectangle ||
                 object.shape == TileObjectShape::Ellipse) &&
                (object.size.x <= 0.0F || object.size.y <= 0.0F))
                result.push_back({TileWorldDiagnosticSeverity::Error, location,
                                  "Collision/object shape has a non-positive size"});
            if (object.type == "prefab") {
                const bool hasAsset = std::any_of(object.properties.begin(), object.properties.end(),
                    [](const TileObjectProperty& property) {
                        return property.name == "prefab_asset" && !property.value.empty();
                    });
                if (!hasAsset)
                    result.push_back({TileWorldDiagnosticSeverity::Error, location,
                                      "Prefab placement has no prefab_asset property"});
            }
            if (object.type == "trigger" && object.shape == TileObjectShape::Point)
                result.push_back({TileWorldDiagnosticSeverity::Warning, location,
                                  "Trigger is a point; use a rectangle or ellipse for direct overlap editing"});
        }
    }
    if (!hasSpawn)
        result.push_back({TileWorldDiagnosticSeverity::Warning, "objects",
                          "Level has no spawn object"});
    if (!hasCheckpoint)
        result.push_back({TileWorldDiagnosticSeverity::Information, "objects",
                          "Level has no checkpoint object"});
    return result;
}

bool TileWorldDesktopWorkspace::request_play_test(std::string* error) {
    const std::vector<TileWorldDiagnostic> diagnostics = validate_level();
    const bool hasError = std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const TileWorldDiagnostic& diagnostic) {
            return diagnostic.severity == TileWorldDiagnosticSeverity::Error;
        });
    if (hasError) return fail(error, "level validation has errors; play test was not started");
    playRequested_ = true;
    return true;
}

bool TileWorldDesktopWorkspace::consume_play_request() noexcept {
    const bool requested = playRequested_;
    playRequested_ = false;
    return requested;
}

TileWorldInspectorFrame TileWorldDesktopWorkspace::build_inspector() const {
    TileWorldInspectorFrame result;
    result.kind = inspectorKind_;
    const TileMap& map = canvas_.session().map();
    const TileMapAuthoringSelection selection = canvas_.session().selection();
    const auto add = [&](std::string name, std::string value, bool editable = true) {
        result.fields.push_back({std::move(name), std::move(value), editable});
    };
    if (inspectorKind_ == TileWorldInspectorKind::Tile ||
        inspectorKind_ == TileWorldInspectorKind::Terrain ||
        inspectorKind_ == TileWorldInspectorKind::Hazard ||
        inspectorKind_ == TileWorldInspectorKind::Trigger) {
        const std::size_t tileIndex = palette_.session().tileset().tiles.empty()
            ? (tile_id(selection.tileValue) == 0U ? 0U : tile_id(selection.tileValue) - 1U)
            : palette_.session().selected_tile();
        const TileSet& set = palette_.session().tileset().tiles.empty()
            ? map.tileset : palette_.session().tileset();
        if (tileIndex < set.tiles.size()) {
            const TileDef& tile = set.tiles[tileIndex];
            result.title = "Tile " + std::to_string(tileIndex + 1U) + " — " + tile.name;
            add("Name", tile.name);
            add("Atlas index", std::to_string(tile.atlasIndex));
            add("Collision", std::to_string(static_cast<unsigned>(tile.collision)));
            add("Terrain", tile.terrain);
            add("Material", tile.material);
            add("Damage / second", std::to_string(tile.damagePerSecond));
            add("Conveyor", std::to_string(tile.conveyorVelocity.x) + ", " +
                            std::to_string(tile.conveyorVelocity.y));
            add("Ladder", tile.ladder ? "true" : "false");
            add("Trigger", tile.trigger);
            add("Animation", std::to_string(tile.animationFrameCount) + " frames @ " +
                             std::to_string(tile.animationTicksPerFrame) + " ticks");
        } else result.title = "Tile Inspector";
        return result;
    }
    if (selection.object) {
        const auto [layerIndex, objectId] = *selection.object;
        if (layerIndex < map.objectLayers.size()) {
            const TileObjectLayer& layer = map.objectLayers[layerIndex];
            const auto found = std::find_if(layer.objects.begin(), layer.objects.end(),
                [&](const TileObject& object) { return object.id == objectId; });
            if (found != layer.objects.end()) {
                result.title = found->name + " — " + found->type;
                add("Name", found->name);
                add("Type", found->type);
                add("Shape", std::to_string(static_cast<unsigned>(found->shape)));
                add("Position", std::to_string(found->position.x) + ", " +
                                std::to_string(found->position.y));
                add("Size", std::to_string(found->size.x) + ", " +
                            std::to_string(found->size.y));
                add("Rotation", std::to_string(found->rotationDegrees));
                for (const TileObjectProperty& property : found->properties)
                    add(property.name, property.value);
                return result;
            }
        }
    }
    result.title = "Tile World Inspector";
    add("Map", map.name, false);
    add("Dimensions", std::to_string(map.pixel_width()) + " x " +
                      std::to_string(map.pixel_height()), false);
    return result;
}

std::vector<TileWorldLayerRow> TileWorldDesktopWorkspace::build_layers() const {
    std::vector<TileWorldLayerRow> result;
    const TileMap& map = canvas_.session().map();
    const TileMapAuthoringSelection selection = canvas_.session().selection();
    result.reserve(map.layers.size() + map.objectLayers.size());
    for (std::size_t index = 0U; index < map.layers.size(); ++index) {
        const TileLayer& layer = map.layers[index];
        result.push_back({false, index, layer.name, layer.kind, layer.visible, layer.locked,
                          layer.collidable, layer.parallaxX, layer.parallaxY,
                          index == selection.tileLayer && !selection.object});
    }
    for (std::size_t index = 0U; index < map.objectLayers.size(); ++index) {
        const TileObjectLayer& layer = map.objectLayers[index];
        result.push_back({true, index, layer.name, TileLayerKind::Visual, layer.visible,
                          layer.locked, false, layer.parallaxX, layer.parallaxY,
                          selection.object && selection.object->first == index});
    }
    return result;
}

std::vector<TileAutotileRuleVisualization>
TileWorldDesktopWorkspace::build_autotile_rules() const {
    std::vector<TileAutotileRuleVisualization> result;
    const TileSet& set = palette_.session().tileset().tiles.empty()
        ? canvas_.session().map().tileset : palette_.session().tileset();
    result.reserve(set.autotileRules.size());
    for (std::size_t index = 0U; index < set.autotileRules.size(); ++index) {
        const TileAutotileRule& rule = set.autotileRules[index];
        TileAutotileRuleVisualization view;
        view.ruleIndex = index;
        view.terrain = rule.terrain;
        view.tileValue = rule.tileValue;
        view.priority = rule.priority;
        view.neighborhood[4] = 2;
        const std::array<std::pair<std::uint8_t, std::size_t>, 4> neighbors{{
            {kTileNeighborNorth, 1U}, {kTileNeighborEast, 5U},
            {kTileNeighborSouth, 7U}, {kTileNeighborWest, 3U}}};
        for (const auto& [bit, cell] : neighbors) {
            if ((rule.requiredMask & bit) != 0U) view.neighborhood[cell] = 1;
            else if ((rule.forbiddenMask & bit) != 0U) view.neighborhood[cell] = -1;
        }
        view.selected = canvas_.session().selection().terrain == rule.terrain;
        result.push_back(std::move(view));
    }
    return result;
}

std::vector<TileParallaxLayerPreview> TileWorldDesktopWorkspace::build_parallax_preview(
    std::uint64_t animationTicks) const {
    std::vector<TileParallaxLayerPreview> result;
    const TileMap& map = canvas_.session().map();
    const TileAabb view = previewCamera_.view_bounds();
    const std::vector<TileRenderItem> visible = build_tile_render_list(map, view, animationTicks);
    result.reserve(map.layers.size() + map.objectLayers.size());
    for (std::size_t index = 0U; index < map.layers.size(); ++index) {
        const TileLayer& layer = map.layers[index];
        const std::uint64_t count = static_cast<std::uint64_t>(std::count_if(
            visible.begin(), visible.end(), [index](const TileRenderItem& item) {
                return item.layerIndex == index;
            }));
        result.push_back({false, index, layer.name,
                          parallax_layer_origin(previewCamera_, layer.parallaxX, layer.parallaxY),
                          count});
    }
    for (std::size_t index = 0U; index < map.objectLayers.size(); ++index) {
        const TileObjectLayer& layer = map.objectLayers[index];
        result.push_back({true, index, layer.name,
                          parallax_layer_origin(previewCamera_, layer.parallaxX, layer.parallaxY),
                          layer.objects.size()});
    }
    return result;
}

void TileWorldDesktopWorkspace::record_recook_overlay() noexcept {
    const TileEditImpact& impact = canvas_.session().last_impact();
    if (!impact.changed) return;
    TileRegion region{};
    if (impact.region) region = *impact.region;
    else if (!canvas_.session().map().layers.empty()) {
        const TileLayer& layer = canvas_.session().map().layers.front();
        if (layer.width != 0U && layer.height != 0U)
            region = {0U, 0U, layer.width - 1U, layer.height - 1U};
    }
    recookOverlay_ = TileRegionalRecookOverlay{
        region, impact.collisionChanged, true, impact.semanticChanged, ++recookGeneration_};
}

TileWorldDesktopFrame TileWorldDesktopWorkspace::frame(
    TileCanvasRect canvasViewport, TileCanvasRect paletteViewport,
    std::uint64_t animationTicks) const {
    TileWorldDesktopFrame result;
    result.canvas = canvas_.frame(canvasViewport, animationTicks);
    result.palette = palette_.frame(paletteViewport);
    result.layers = build_layers();
    result.inspector = build_inspector();
    if (showAutotileRules_) result.autotileRules = build_autotile_rules();
    if (showParallaxPreview_) result.parallaxLayers = build_parallax_preview(animationTicks);
    if (showChunkDiagnostics_)
        result.chunks = build_tile_chunk_diagnostics(canvas_.session().map());
    result.recookOverlay = recookOverlay_;
    result.validation = validate_level();
    result.playRequested = playRequested_;
    result.validForPlay = std::none_of(result.validation.begin(), result.validation.end(),
        [](const TileWorldDiagnostic& diagnostic) {
            return diagnostic.severity == TileWorldDiagnosticSeverity::Error;
        });
    return result;
}

} // namespace dve
