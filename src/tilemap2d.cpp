#include "dve/tilemap2d.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>

namespace dve {

namespace {

constexpr std::uint64_t kMaxTileLayerCells = 16ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool fits_u32(std::uint64_t value) noexcept {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool valid_layer_size(std::uint64_t width, std::uint64_t height) noexcept {
    return width > 0 && height > 0 && fits_u32(width) && fits_u32(height) &&
           width <= kMaxTileLayerCells / height;
}

void append_uint(std::string& out, std::uint64_t v) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, result.ptr);
}

void append_float(std::string& out, float v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(v));
    out.append(buffer);
}

[[nodiscard]] bool parse_uint(std::string_view token, std::uint64_t& out) {
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_float(std::string_view token, float& out) {
    std::string copy(token);
    char* endPtr = nullptr;
    const float value = std::strtof(copy.c_str(), &endPtr);
    if (endPtr == copy.c_str() || *endPtr != '\0' || !std::isfinite(value)) return false;
    out = value;
    return true;
}

void hash_bytes(std::uint64_t& state, std::string_view bytes) noexcept {
    for (const char c : bytes) {
        state ^= static_cast<std::uint8_t>(c);
        state *= 0x100000001b3ULL;
    }
}

std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

[[nodiscard]] std::int64_t floor_div(float value, std::uint32_t divisor) noexcept {
    return static_cast<std::int64_t>(std::floor(value / static_cast<float>(divisor)));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TileSet / TileLayer / TileMap basics
// ---------------------------------------------------------------------------------------------

namespace {

[[nodiscard]] bool parse_int(std::string_view token, std::int64_t& out) {
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

void append_int(std::string& out, std::int64_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

[[nodiscard]] bool canonical_token(std::string_view value) noexcept {
    if (value.empty()) return true;
    return value.find_first_of(" \t\r\n") == std::string_view::npos && value != "-";
}

[[nodiscard]] std::string_view token_or_dash(std::string_view value) noexcept {
    return value.empty() ? std::string_view{"-"} : value;
}

[[nodiscard]] std::string token_value(std::string_view value) {
    return value == "-" ? std::string{} : std::string(value);
}

[[nodiscard]] bool finite_vec(TileVec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] std::uint32_t popcount4(std::uint8_t value) noexcept {
    value = static_cast<std::uint8_t>(value & 0x0FU);
    std::uint32_t count = 0;
    while (value != 0U) {
        count += value & 1U;
        value = static_cast<std::uint8_t>(value >> 1U);
    }
    return count;
}

[[nodiscard]] bool tile_def_valid(const TileDef& tile, std::string* error) {
    if (!canonical_token(tile.terrain) || !canonical_token(tile.material) ||
        !canonical_token(tile.trigger)) {
        if (error) *error = "tile semantic identifiers must be whitespace-free tokens";
        return false;
    }
    if (!std::isfinite(tile.damagePerSecond) || tile.damagePerSecond < 0.0F ||
        !finite_vec(tile.conveyorVelocity)) {
        if (error) *error = "tile semantic numeric values are invalid";
        return false;
    }
    if (tile.animationFrameCount == 0U) {
        if (error) *error = "tile animation frame count must be positive";
        return false;
    }
    if (tile.animationFrameCount > 1U && tile.animationTicksPerFrame == 0U) {
        if (error) *error = "animated tile ticks-per-frame must be positive";
        return false;
    }
    return true;
}

void append_tile_v2(std::string& out, const TileDef& tile, std::string_view prefix) {
    out += prefix;
    out += ' ';
    append_uint(out, tile.atlasIndex);
    out += ' ';
    append_uint(out, static_cast<std::uint64_t>(tile.collision));
    out += ' ';
    append_float(out, tile.damagePerSecond);
    out += ' ';
    append_float(out, tile.conveyorVelocity.x);
    out += ' ';
    append_float(out, tile.conveyorVelocity.y);
    out += tile.ladder ? " 1 " : " 0 ";
    append_uint(out, tile.animationFirstAtlasIndex);
    out += ' ';
    append_uint(out, tile.animationFrameCount);
    out += ' ';
    append_uint(out, tile.animationTicksPerFrame);
    out += ' ';
    out += token_or_dash(tile.terrain);
    out += ' ';
    out += token_or_dash(tile.material);
    out += ' ';
    out += token_or_dash(tile.trigger);
    out += ' ';
    out += tile.name;
    out += '\n';
}

[[nodiscard]] bool parse_tile_v2(std::string_view line,
                                 const std::vector<std::string_view>& tokens,
                                 TileDef& def) {
    if (tokens.size() < 14U) return false;
    std::uint64_t atlas{}, collision{}, ladder{}, animationFirst{}, animationCount{}, animationTicks{};
    float damage{}, conveyorX{}, conveyorY{};
    if (!parse_uint(tokens[1], atlas) || !fits_u32(atlas) ||
        !parse_uint(tokens[2], collision) ||
        collision > static_cast<std::uint64_t>(TileCollision::SlopeUpLeft) ||
        !parse_float(tokens[3], damage) || !parse_float(tokens[4], conveyorX) ||
        !parse_float(tokens[5], conveyorY) || !parse_uint(tokens[6], ladder) || ladder > 1U ||
        !parse_uint(tokens[7], animationFirst) || !fits_u32(animationFirst) ||
        !parse_uint(tokens[8], animationCount) || !fits_u32(animationCount) ||
        !parse_uint(tokens[9], animationTicks) || !fits_u32(animationTicks)) {
        return false;
    }
    def.atlasIndex = static_cast<std::uint32_t>(atlas);
    def.collision = static_cast<TileCollision>(collision);
    def.damagePerSecond = damage;
    def.conveyorVelocity = {conveyorX, conveyorY};
    def.ladder = ladder != 0U;
    def.animationFirstAtlasIndex = static_cast<std::uint32_t>(animationFirst);
    def.animationFrameCount = static_cast<std::uint32_t>(animationCount);
    def.animationTicksPerFrame = static_cast<std::uint32_t>(animationTicks);
    def.terrain = token_value(tokens[10]);
    def.material = token_value(tokens[11]);
    def.trigger = token_value(tokens[12]);
    const std::size_t namePos = line.find(tokens[13]);
    def.name = namePos == std::string_view::npos ? std::string{} : std::string(line.substr(namePos));
    return tile_def_valid(def, nullptr);
}

} // namespace

TileCollision TileSet::collision_for(std::uint32_t tileValue) const noexcept {
    const std::uint32_t id = tile_id(tileValue);
    if (id == 0) return TileCollision::Empty;
    const std::uint32_t index = id - 1;
    if (index >= tiles.size()) return TileCollision::Empty;
    return tiles[index].collision;
}

std::uint32_t TileSet::atlas_index_for(std::uint32_t tileValue, std::uint64_t ticks) const noexcept {
    const std::uint32_t id = tile_id(tileValue);
    if (id == 0U || id > tiles.size()) return 0U;
    const TileDef& tile = tiles[id - 1U];
    std::uint32_t atlas = tile.atlasIndex;
    if (tile.animationFrameCount > 1U && tile.animationTicksPerFrame > 0U) {
        const std::uint64_t frame = (ticks / tile.animationTicksPerFrame) % tile.animationFrameCount;
        atlas = tile.animationFirstAtlasIndex + static_cast<std::uint32_t>(frame);
    }
    return atlas;
}

std::uint32_t TileSet::autotile_value(std::string_view terrain,
                                      std::uint8_t neighborMask) const noexcept {
    const TileAutotileRule* best = nullptr;
    std::uint32_t bestSpecificity = 0U;
    for (const TileAutotileRule& rule : autotileRules) {
        if (rule.terrain != terrain) continue;
        if ((neighborMask & rule.requiredMask) != rule.requiredMask) continue;
        if ((neighborMask & rule.forbiddenMask) != 0U) continue;
        const std::uint32_t specificity = popcount4(static_cast<std::uint8_t>(
            rule.requiredMask | rule.forbiddenMask));
        if (best == nullptr || rule.priority > best->priority ||
            (rule.priority == best->priority && specificity > bestSpecificity)) {
            best = &rule;
            bestSpecificity = specificity;
        }
    }
    if (best != nullptr) return best->tileValue;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (tiles[i].terrain == terrain) return static_cast<std::uint32_t>(i + 1U);
    }
    return 0U;
}

bool TileSet::validate(std::string* error) const {
    if (tileWidth == 0 || tileHeight == 0) {
        if (error) *error = "tileset tile size must be positive";
        return false;
    }
    if (columns == 0) {
        if (error) *error = "tileset columns must be positive";
        return false;
    }
    if ((textureWidth == 0U) != (textureHeight == 0U)) {
        if (error) *error = "tileset texture dimensions must both be zero or both be positive";
        return false;
    }
    std::uint64_t sourceCellCount = std::numeric_limits<std::uint64_t>::max();
    if (textureWidth > 0U) {
        const std::uint64_t requiredWidth = static_cast<std::uint64_t>(margin) * 2U + tileWidth;
        const std::uint64_t requiredHeight = static_cast<std::uint64_t>(margin) * 2U + tileHeight;
        if (requiredWidth > textureWidth || requiredHeight > textureHeight) {
            if (error) *error = "tileset texture is smaller than the grid margin and one tile";
            return false;
        }
        const std::uint64_t strideX = static_cast<std::uint64_t>(tileWidth) + spacing;
        const std::uint64_t strideY = static_cast<std::uint64_t>(tileHeight) + spacing;
        const std::uint64_t availableWidth = textureWidth - static_cast<std::uint64_t>(margin) * 2U;
        const std::uint64_t availableHeight = textureHeight - static_cast<std::uint64_t>(margin) * 2U;
        const std::uint64_t availableColumns = 1U + (availableWidth - tileWidth) / strideX;
        const std::uint64_t availableRows = 1U + (availableHeight - tileHeight) / strideY;
        if (columns > availableColumns) {
            if (error) *error = "tileset columns exceed the sliced source texture";
            return false;
        }
        sourceCellCount = availableColumns * availableRows;
    }
    if (tiles.size() > kTileIdMask) {
        if (error) *error = "too many tile definitions";
        return false;
    }
    for (const TileDef& tile : tiles) {
        if (!tile_def_valid(tile, error)) return false;
        if (tile.atlasIndex >= sourceCellCount || tile.animationFirstAtlasIndex >= sourceCellCount ||
            static_cast<std::uint64_t>(tile.animationFirstAtlasIndex) + tile.animationFrameCount >
                sourceCellCount) {
            if (error) *error = "tile atlas or animation range exceeds the sliced source texture";
            return false;
        }
    }
    for (const TileAutotileRule& rule : autotileRules) {
        if (rule.terrain.empty() || !canonical_token(rule.terrain)) {
            if (error) *error = "autotile terrain must be a non-empty token";
            return false;
        }
        if ((rule.requiredMask & 0xF0U) != 0U || (rule.forbiddenMask & 0xF0U) != 0U ||
            (rule.requiredMask & rule.forbiddenMask) != 0U) {
            if (error) *error = "autotile masks are invalid";
            return false;
        }
        const std::uint32_t id = tile_id(rule.tileValue);
        if (id == 0U || id > tiles.size()) {
            if (error) *error = "autotile rule references a missing tile";
            return false;
        }
        if (tiles[id - 1U].terrain != rule.terrain) {
            if (error) *error = "autotile output tile terrain mismatch";
            return false;
        }
    }
    return true;
}

std::string TileSet::serialize() const {
    std::string out;
    out.reserve(4096U);
    out += "dvetileset 1\n";
    out += "name "; out += name; out += '\n';
    out += "texture "; out += textureAsset; out += '\n';
    out += "texture_size "; append_uint(out, textureWidth); out += ' '; append_uint(out, textureHeight); out += '\n';
    out += "tile_size "; append_uint(out, tileWidth); out += ' '; append_uint(out, tileHeight); out += '\n';
    out += "grid "; append_uint(out, margin); out += ' '; append_uint(out, spacing); out += '\n';
    out += "columns "; append_uint(out, columns); out += '\n';
    out += "tiles "; append_uint(out, tiles.size()); out += '\n';
    for (const TileDef& tile : tiles) append_tile_v2(out, tile, "tile");
    out += "rules "; append_uint(out, autotileRules.size()); out += '\n';
    for (const TileAutotileRule& rule : autotileRules) {
        out += "rule "; append_int(out, rule.priority); out += ' ';
        append_uint(out, rule.requiredMask); out += ' ';
        append_uint(out, rule.forbiddenMask); out += ' ';
        append_uint(out, rule.tileValue); out += ' '; out += rule.terrain; out += '\n';
    }
    return out;
}

bool TileSet::parse(std::string_view text, TileSet& out, std::string* error) {
    out = TileSet{};
    out.tiles.clear();
    out.autotileRules.clear();
    std::istringstream stream{std::string(text)};
    std::string rawLine;
    bool sawMagic = false;
    std::uint64_t expectedTiles = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t expectedRules = std::numeric_limits<std::uint64_t>::max();
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    auto tail_after = [](std::string_view line, std::string_view key) {
        const std::size_t start = key.size() + 1U;
        return line.size() > start ? std::string(line.substr(start)) : std::string{};
    };
    while (std::getline(stream, rawLine)) {
        std::string_view line = rawLine;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (line.empty()) continue;
        const auto tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string_view key = tokens.front();
        if (!sawMagic) {
            if (tokens.size() != 2U || key != "dvetileset" || tokens[1] != "1")
                return fail("unsupported or missing dvetileset version");
            sawMagic = true;
        } else if (key == "name") {
            out.name = tail_after(line, key);
        } else if (key == "texture") {
            out.textureAsset = tail_after(line, key);
        } else if (key == "texture_size") {
            std::uint64_t w{}, h{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], w) || !parse_uint(tokens[2], h) ||
                !fits_u32(w) || !fits_u32(h)) return fail("bad tileset texture_size");
            out.textureWidth = static_cast<std::uint32_t>(w);
            out.textureHeight = static_cast<std::uint32_t>(h);
        } else if (key == "tile_size") {
            std::uint64_t w{}, h{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], w) || !parse_uint(tokens[2], h) ||
                !fits_u32(w) || !fits_u32(h)) return fail("bad tileset tile_size");
            out.tileWidth = static_cast<std::uint32_t>(w);
            out.tileHeight = static_cast<std::uint32_t>(h);
        } else if (key == "grid") {
            std::uint64_t margin{}, spacing{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], margin) ||
                !parse_uint(tokens[2], spacing) || !fits_u32(margin) || !fits_u32(spacing)) {
                return fail("bad tileset grid");
            }
            out.margin = static_cast<std::uint32_t>(margin);
            out.spacing = static_cast<std::uint32_t>(spacing);
        } else if (key == "columns") {
            std::uint64_t columns{};
            if (tokens.size() != 2U || !parse_uint(tokens[1], columns) || !fits_u32(columns))
                return fail("bad tileset columns");
            out.columns = static_cast<std::uint32_t>(columns);
        } else if (key == "tiles") {
            if (tokens.size() != 2U || !parse_uint(tokens[1], expectedTiles) ||
                expectedTiles > kTileIdMask) return fail("bad tileset tile count");
        } else if (key == "tile") {
            TileDef tile;
            if (!parse_tile_v2(line, tokens, tile)) return fail("bad tileset tile definition");
            out.tiles.push_back(std::move(tile));
        } else if (key == "rules") {
            if (tokens.size() != 2U || !parse_uint(tokens[1], expectedRules) ||
                !fits_u32(expectedRules)) return fail("bad autotile rule count");
        } else if (key == "rule") {
            std::int64_t priority{};
            std::uint64_t required{}, forbidden{}, tileValue{};
            if (tokens.size() != 6U || !parse_int(tokens[1], priority) ||
                priority < std::numeric_limits<std::int32_t>::min() ||
                priority > std::numeric_limits<std::int32_t>::max() ||
                !parse_uint(tokens[2], required) || required > 15U ||
                !parse_uint(tokens[3], forbidden) || forbidden > 15U ||
                !parse_uint(tokens[4], tileValue) || !fits_u32(tileValue)) {
                return fail("bad autotile rule");
            }
            out.autotileRules.push_back(TileAutotileRule{
                std::string(tokens[5]), static_cast<std::uint8_t>(required),
                static_cast<std::uint8_t>(forbidden), static_cast<std::uint32_t>(tileValue),
                static_cast<std::int32_t>(priority)});
        } else {
            return fail("unknown dvetileset key");
        }
    }
    if (!sawMagic) return fail("empty or non-dvetileset input");
    if (expectedTiles != std::numeric_limits<std::uint64_t>::max() &&
        expectedTiles != out.tiles.size()) return fail("tileset tile count mismatch");
    if (expectedRules != std::numeric_limits<std::uint64_t>::max() &&
        expectedRules != out.autotileRules.size()) return fail("autotile rule count mismatch");
    return out.validate(error);
}

std::uint64_t TileSet::content_hash() const {
    std::uint64_t state = 0xcbf29ce484222325ULL;
    hash_bytes(state, serialize());
    return state;
}

std::uint32_t TileLayer::at(std::uint32_t col, std::uint32_t row) const noexcept {
    if (col >= width || row >= height) return 0;
    return tiles[static_cast<std::size_t>(row) * width + col];
}

void TileLayer::set(std::uint32_t col, std::uint32_t row, std::uint32_t value) noexcept {
    if (col >= width || row >= height) return;
    tiles[static_cast<std::size_t>(row) * width + col] = value;
}

bool TileLayer::validate(std::string* error) const {
    if (!valid_layer_size(width, height)) {
        if (error) *error = "layer dimensions are invalid or exceed the 16M-cell limit";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (tiles.size() != expected) {
        if (error) *error = "layer tile count mismatch";
        return false;
    }
    if (!(parallaxX >= 0.0F && parallaxX <= 8.0F) ||
        !(parallaxY >= 0.0F && parallaxY <= 8.0F) ||
        !std::isfinite(parallaxX) || !std::isfinite(parallaxY)) {
        if (error) *error = "layer parallax out of range";
        return false;
    }
    if (kind > TileLayerKind::Trigger) {
        if (error) *error = "tile layer kind is invalid";
        return false;
    }
    return true;
}

bool TileObjectLayer::validate(std::string* error) const {
    if (!(parallaxX >= 0.0F && parallaxX <= 8.0F) ||
        !(parallaxY >= 0.0F && parallaxY <= 8.0F) ||
        !std::isfinite(parallaxX) || !std::isfinite(parallaxY)) {
        if (error) *error = "object-layer parallax out of range";
        return false;
    }
    std::set<std::uint64_t> ids;
    for (const TileObject& object : objects) {
        if (object.id == 0U || !ids.insert(object.id).second) {
            if (error) *error = "object ids must be nonzero and unique within a layer";
            return false;
        }
        if (!canonical_token(object.type) || !finite_vec(object.position) ||
            !finite_vec(object.size) || object.size.x < 0.0F || object.size.y < 0.0F ||
            !std::isfinite(object.rotationDegrees) || object.shape > TileObjectShape::Ellipse) {
            if (error) *error = "tile object values are invalid";
            return false;
        }
        std::set<std::string> propertyNames;
        for (const TileObjectProperty& property : object.properties) {
            if (property.name.empty() || !canonical_token(property.name) ||
                !propertyNames.insert(property.name).second) {
                if (error) *error = "object property names must be unique non-empty tokens";
                return false;
            }
        }
    }
    return true;
}

bool TileMap::validate(std::string* error) const {
    if (tileWidth == 0 || tileHeight == 0) {
        if (error) *error = "map tile size must be positive";
        return false;
    }
    if (!tileset.validate(error)) return false;
    if (tileset.tileWidth != tileWidth || tileset.tileHeight != tileHeight) {
        if (error) *error = "map and tileset tile sizes must match";
        return false;
    }
    if (layers.empty()) {
        if (error) *error = "map has no tile layers";
        return false;
    }
    for (const TileLayer& layer : layers) {
        if (!layer.validate(error)) return false;
        if (layer.width > std::numeric_limits<std::uint32_t>::max() / tileWidth ||
            layer.height > std::numeric_limits<std::uint32_t>::max() / tileHeight) {
            if (error) *error = "map pixel dimensions overflow 32-bit coordinates";
            return false;
        }
        for (const std::uint32_t value : layer.tiles) {
            if (tile_id(value) > tileset.tiles.size()) {
                if (error) *error = "layer references missing tile id";
                return false;
            }
        }
    }
    std::set<std::uint64_t> objectIds;
    for (const TileObjectLayer& layer : objectLayers) {
        if (!layer.validate(error)) return false;
        for (const TileObject& object : layer.objects) {
            if (!objectIds.insert(object.id).second) {
                if (error) *error = "object ids must be unique across the map";
                return false;
            }
        }
    }
    return true;
}

std::uint32_t TileMap::pixel_width() const noexcept {
    std::uint32_t maxW = 0;
    for (const auto& layer : layers) maxW = std::max(maxW, layer.width);
    return maxW * tileWidth;
}

std::uint32_t TileMap::pixel_height() const noexcept {
    std::uint32_t maxH = 0;
    for (const auto& layer : layers) maxH = std::max(maxH, layer.height);
    return maxH * tileHeight;
}

// ---------------------------------------------------------------------------------------------
// Serialization (.dvetilemap v2, with v1 migration)
// ---------------------------------------------------------------------------------------------

std::string TileMap::serialize() const {
    std::string out;
    out.reserve(4096U);
    out += "dvetilemap 2\n";
    out += "name "; out += name; out += '\n';
    out += "tile_size "; append_uint(out, tileWidth); out += ' '; append_uint(out, tileHeight); out += '\n';
    out += "tileset_asset "; out += tilesetAsset; out += '\n';
    out += "tileset\n";
    out += "ts_name "; out += tileset.name; out += '\n';
    out += "ts_texture "; out += tileset.textureAsset; out += '\n';
    out += "ts_texture_size "; append_uint(out, tileset.textureWidth); out += ' '; append_uint(out, tileset.textureHeight); out += '\n';
    out += "ts_tile_size "; append_uint(out, tileset.tileWidth); out += ' '; append_uint(out, tileset.tileHeight); out += '\n';
    out += "ts_grid "; append_uint(out, tileset.margin); out += ' '; append_uint(out, tileset.spacing); out += '\n';
    out += "ts_columns "; append_uint(out, tileset.columns); out += '\n';
    out += "ts_tiles "; append_uint(out, tileset.tiles.size()); out += '\n';
    for (const TileDef& tile : tileset.tiles) append_tile_v2(out, tile, "tile");
    out += "ts_rules "; append_uint(out, tileset.autotileRules.size()); out += '\n';
    for (const TileAutotileRule& rule : tileset.autotileRules) {
        out += "rule "; append_int(out, rule.priority); out += ' ';
        append_uint(out, rule.requiredMask); out += ' ';
        append_uint(out, rule.forbiddenMask); out += ' ';
        append_uint(out, rule.tileValue); out += ' '; out += rule.terrain; out += '\n';
    }
    out += "layers "; append_uint(out, layers.size()); out += '\n';
    for (const TileLayer& layer : layers) {
        out += "layer "; append_uint(out, layer.width); out += ' '; append_uint(out, layer.height); out += '\n';
        out += "l_name "; out += layer.name; out += '\n';
        out += "l_kind "; append_uint(out, static_cast<std::uint64_t>(layer.kind)); out += '\n';
        out += "l_parallax "; append_float(out, layer.parallaxX); out += ' '; append_float(out, layer.parallaxY); out += '\n';
        out += "l_flags "; out += layer.visible ? "1" : "0"; out += ' ';
        out += layer.collidable ? "1" : "0"; out += ' '; out += layer.locked ? "1\n" : "0\n";
        for (std::uint32_t row = 0; row < layer.height; ++row) {
            out += 'r';
            for (std::uint32_t col = 0; col < layer.width; ++col) {
                out += ' '; append_uint(out, layer.at(col, row));
            }
            out += '\n';
        }
    }
    out += "object_layers "; append_uint(out, objectLayers.size()); out += '\n';
    for (const TileObjectLayer& layer : objectLayers) {
        out += "object_layer\n";
        out += "ol_name "; out += layer.name; out += '\n';
        out += "ol_parallax "; append_float(out, layer.parallaxX); out += ' '; append_float(out, layer.parallaxY); out += '\n';
        out += "ol_flags "; out += layer.visible ? "1" : "0"; out += ' '; out += layer.locked ? "1\n" : "0\n";
        out += "ol_objects "; append_uint(out, layer.objects.size()); out += '\n';
        for (const TileObject& object : layer.objects) {
            out += "object "; append_uint(out, object.id); out += ' ';
            append_uint(out, static_cast<std::uint64_t>(object.shape)); out += ' ';
            append_float(out, object.position.x); out += ' '; append_float(out, object.position.y); out += ' ';
            append_float(out, object.size.x); out += ' '; append_float(out, object.size.y); out += ' ';
            append_float(out, object.rotationDegrees); out += ' '; out += token_or_dash(object.type); out += ' ';
            out += object.name; out += '\n';
            out += "o_props "; append_uint(out, object.properties.size()); out += '\n';
            for (const TileObjectProperty& property : object.properties) {
                out += "prop "; out += property.name; out += ' '; out += property.value; out += '\n';
            }
        }
    }
    return out;
}

bool TileMap::parse(std::string_view text, TileMap& out, std::string* error) {
    out = TileMap{};
    out.layers.clear();
    out.objectLayers.clear();
    out.tileset.tiles.clear();
    out.tileset.autotileRules.clear();
    std::istringstream stream{std::string(text)};
    std::string rawLine;
    std::uint32_t version = 0U;
    std::uint64_t expectedTileCount = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t expectedRuleCount = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t expectedLayerCount = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t expectedObjectLayerCount = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t expectedObjects = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t expectedProperties = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t readObjects = 0U;
    std::uint64_t readProperties = 0U;
    std::uint32_t currentLayerRows = 0U;
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    auto tail_after = [](std::string_view line, std::string_view key) {
        const std::size_t start = key.size() + 1U;
        return line.size() > start ? std::string(line.substr(start)) : std::string{};
    };
    auto finish_properties = [&]() {
        return expectedProperties == std::numeric_limits<std::uint64_t>::max() ||
            readProperties == expectedProperties;
    };
    auto finish_objects = [&]() {
        return expectedObjects == std::numeric_limits<std::uint64_t>::max() ||
            readObjects == expectedObjects;
    };
    while (std::getline(stream, rawLine)) {
        std::string_view line = rawLine;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (line.empty()) continue;
        const auto tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string_view key = tokens.front();
        if (version == 0U) {
            std::uint64_t parsedVersion{};
            if (key != "dvetilemap" || tokens.size() != 2U ||
                !parse_uint(tokens[1], parsedVersion) || (parsedVersion != 1U && parsedVersion != 2U)) {
                return fail("unsupported or missing dvetilemap version");
            }
            version = static_cast<std::uint32_t>(parsedVersion);
            continue;
        }
        if (key == "name") {
            out.name = tail_after(line, key);
        } else if (key == "tile_size") {
            std::uint64_t w{}, h{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], w) || !parse_uint(tokens[2], h) ||
                !fits_u32(w) || !fits_u32(h)) return fail("bad tile_size");
            out.tileWidth = static_cast<std::uint32_t>(w);
            out.tileHeight = static_cast<std::uint32_t>(h);
        } else if (key == "tileset_asset") {
            if (version < 2U) return fail("tileset_asset is not valid in v1");
            out.tilesetAsset = tail_after(line, key);
        } else if (key == "tileset") {
            // block marker
        } else if (key == "ts_name") {
            out.tileset.name = tail_after(line, key);
        } else if (key == "ts_texture") {
            out.tileset.textureAsset = tail_after(line, key);
        } else if (key == "ts_texture_size") {
            if (version < 2U) return fail("ts_texture_size is not valid in v1");
            std::uint64_t w{}, h{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], w) || !parse_uint(tokens[2], h) ||
                !fits_u32(w) || !fits_u32(h)) return fail("bad ts_texture_size");
            out.tileset.textureWidth = static_cast<std::uint32_t>(w);
            out.tileset.textureHeight = static_cast<std::uint32_t>(h);
        } else if (key == "ts_tile_size") {
            std::uint64_t w{}, h{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], w) || !parse_uint(tokens[2], h) ||
                !fits_u32(w) || !fits_u32(h)) return fail("bad ts_tile_size");
            out.tileset.tileWidth = static_cast<std::uint32_t>(w);
            out.tileset.tileHeight = static_cast<std::uint32_t>(h);
        } else if (key == "ts_grid") {
            if (version < 2U) return fail("ts_grid is not valid in v1");
            std::uint64_t margin{}, spacing{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], margin) ||
                !parse_uint(tokens[2], spacing) || !fits_u32(margin) || !fits_u32(spacing)) {
                return fail("bad ts_grid");
            }
            out.tileset.margin = static_cast<std::uint32_t>(margin);
            out.tileset.spacing = static_cast<std::uint32_t>(spacing);
        } else if (key == "ts_columns") {
            std::uint64_t columns{};
            if (tokens.size() != 2U || !parse_uint(tokens[1], columns) || !fits_u32(columns))
                return fail("bad ts_columns");
            out.tileset.columns = static_cast<std::uint32_t>(columns);
        } else if (key == "ts_tiles") {
            if (tokens.size() != 2U || !parse_uint(tokens[1], expectedTileCount) ||
                expectedTileCount > kTileIdMask) return fail("bad ts_tiles");
        } else if (key == "tile") {
            TileDef tile;
            if (version == 1U) {
                std::uint64_t atlas{}, collision{};
                if (tokens.size() < 3U || !parse_uint(tokens[1], atlas) || !fits_u32(atlas) ||
                    !parse_uint(tokens[2], collision) ||
                    collision > static_cast<std::uint64_t>(TileCollision::SlopeUpLeft)) {
                    return fail("bad v1 tile definition");
                }
                tile.atlasIndex = static_cast<std::uint32_t>(atlas);
                tile.collision = static_cast<TileCollision>(collision);
                tile.animationFirstAtlasIndex = tile.atlasIndex;
                if (tokens.size() >= 4U) {
                    const std::size_t pos = line.find(tokens[3]);
                    if (pos != std::string_view::npos) tile.name = std::string(line.substr(pos));
                }
            } else if (!parse_tile_v2(line, tokens, tile)) {
                return fail("bad v2 tile definition");
            }
            out.tileset.tiles.push_back(std::move(tile));
        } else if (key == "ts_rules") {
            if (version < 2U || tokens.size() != 2U || !parse_uint(tokens[1], expectedRuleCount) ||
                !fits_u32(expectedRuleCount)) return fail("bad ts_rules");
        } else if (key == "rule") {
            std::int64_t priority{};
            std::uint64_t required{}, forbidden{}, tileValue{};
            if (version < 2U || tokens.size() != 6U || !parse_int(tokens[1], priority) ||
                priority < std::numeric_limits<std::int32_t>::min() ||
                priority > std::numeric_limits<std::int32_t>::max() ||
                !parse_uint(tokens[2], required) || required > 15U ||
                !parse_uint(tokens[3], forbidden) || forbidden > 15U ||
                !parse_uint(tokens[4], tileValue) || !fits_u32(tileValue)) return fail("bad rule");
            out.tileset.autotileRules.push_back(TileAutotileRule{
                std::string(tokens[5]), static_cast<std::uint8_t>(required),
                static_cast<std::uint8_t>(forbidden), static_cast<std::uint32_t>(tileValue),
                static_cast<std::int32_t>(priority)});
        } else if (key == "layers") {
            if (tokens.size() != 2U || !parse_uint(tokens[1], expectedLayerCount) ||
                !fits_u32(expectedLayerCount)) return fail("bad layers count");
        } else if (key == "layer") {
            if (!out.layers.empty() && currentLayerRows != out.layers.back().height)
                return fail("previous layer row count mismatch");
            std::uint64_t w{}, h{};
            if (tokens.size() != 3U || !parse_uint(tokens[1], w) || !parse_uint(tokens[2], h) ||
                !valid_layer_size(w, h)) return fail("bad layer dims");
            TileLayer layer;
            layer.width = static_cast<std::uint32_t>(w);
            layer.height = static_cast<std::uint32_t>(h);
            layer.tiles.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            out.layers.push_back(std::move(layer));
            currentLayerRows = 0U;
        } else if (key == "l_name") {
            if (out.layers.empty()) return fail("l_name outside layer");
            out.layers.back().name = tail_after(line, key);
        } else if (key == "l_kind") {
            std::uint64_t kind{};
            if (version < 2U || out.layers.empty() || tokens.size() != 2U ||
                !parse_uint(tokens[1], kind) || kind > static_cast<std::uint64_t>(TileLayerKind::Trigger))
                return fail("bad l_kind");
            out.layers.back().kind = static_cast<TileLayerKind>(kind);
        } else if (key == "l_parallax") {
            float px{}, py{};
            if (out.layers.empty() || tokens.size() != 3U || !parse_float(tokens[1], px) ||
                !parse_float(tokens[2], py)) return fail("bad l_parallax");
            out.layers.back().parallaxX = px;
            out.layers.back().parallaxY = py;
        } else if (key == "l_flags") {
            if (out.layers.empty() || (tokens.size() != 3U && tokens.size() != 4U) ||
                (tokens[1] != "0" && tokens[1] != "1") ||
                (tokens[2] != "0" && tokens[2] != "1") ||
                (tokens.size() == 4U && tokens[3] != "0" && tokens[3] != "1")) {
                return fail("bad l_flags");
            }
            out.layers.back().visible = tokens[1] == "1";
            out.layers.back().collidable = tokens[2] == "1";
            out.layers.back().locked = tokens.size() == 4U && tokens[3] == "1";
        } else if (key == "r") {
            if (out.layers.empty()) return fail("row outside layer");
            TileLayer& layer = out.layers.back();
            if (currentLayerRows >= layer.height || tokens.size() - 1U != layer.width)
                return fail("row size mismatch");
            for (std::size_t i = 1U; i < tokens.size(); ++i) {
                std::uint64_t value{};
                if (!parse_uint(tokens[i], value) || !fits_u32(value)) return fail("bad tile value");
                layer.tiles.push_back(static_cast<std::uint32_t>(value));
            }
            ++currentLayerRows;
        } else if (key == "object_layers") {
            if (version < 2U || !finish_properties() || !finish_objects() || tokens.size() != 2U ||
                !parse_uint(tokens[1], expectedObjectLayerCount) || !fits_u32(expectedObjectLayerCount))
                return fail("bad object_layers");
        } else if (key == "object_layer") {
            if (version < 2U || !finish_properties() || !finish_objects())
                return fail("unfinished object layer");
            out.objectLayers.push_back(TileObjectLayer{});
            expectedObjects = std::numeric_limits<std::uint64_t>::max();
            readObjects = 0U;
            expectedProperties = std::numeric_limits<std::uint64_t>::max();
            readProperties = 0U;
        } else if (key == "ol_name") {
            if (out.objectLayers.empty()) return fail("ol_name outside object layer");
            out.objectLayers.back().name = tail_after(line, key);
        } else if (key == "ol_parallax") {
            float px{}, py{};
            if (out.objectLayers.empty() || tokens.size() != 3U || !parse_float(tokens[1], px) ||
                !parse_float(tokens[2], py)) return fail("bad ol_parallax");
            out.objectLayers.back().parallaxX = px;
            out.objectLayers.back().parallaxY = py;
        } else if (key == "ol_flags") {
            if (out.objectLayers.empty() || tokens.size() != 3U ||
                (tokens[1] != "0" && tokens[1] != "1") ||
                (tokens[2] != "0" && tokens[2] != "1")) return fail("bad ol_flags");
            out.objectLayers.back().visible = tokens[1] == "1";
            out.objectLayers.back().locked = tokens[2] == "1";
        } else if (key == "ol_objects") {
            if (out.objectLayers.empty() || tokens.size() != 2U || !parse_uint(tokens[1], expectedObjects) ||
                !fits_u32(expectedObjects)) return fail("bad ol_objects");
            readObjects = 0U;
        } else if (key == "object") {
            if (out.objectLayers.empty() || !finish_properties()) return fail("bad object placement");
            std::uint64_t id{}, shape{};
            float x{}, y{}, width{}, height{}, rotation{};
            if (tokens.size() < 10U || !parse_uint(tokens[1], id) || id == 0U ||
                !parse_uint(tokens[2], shape) || shape > static_cast<std::uint64_t>(TileObjectShape::Ellipse) ||
                !parse_float(tokens[3], x) || !parse_float(tokens[4], y) ||
                !parse_float(tokens[5], width) || !parse_float(tokens[6], height) ||
                !parse_float(tokens[7], rotation)) return fail("bad object");
            TileObject object;
            object.id = id;
            object.shape = static_cast<TileObjectShape>(shape);
            object.position = {x, y};
            object.size = {width, height};
            object.rotationDegrees = rotation;
            object.type = token_value(tokens[8]);
            const std::size_t namePos = line.find(tokens[9]);
            object.name = namePos == std::string_view::npos ? std::string{} : std::string(line.substr(namePos));
            out.objectLayers.back().objects.push_back(std::move(object));
            ++readObjects;
            expectedProperties = std::numeric_limits<std::uint64_t>::max();
            readProperties = 0U;
        } else if (key == "o_props") {
            if (out.objectLayers.empty() || out.objectLayers.back().objects.empty() ||
                tokens.size() != 2U || !parse_uint(tokens[1], expectedProperties) ||
                !fits_u32(expectedProperties)) return fail("bad o_props");
            readProperties = 0U;
        } else if (key == "prop") {
            if (out.objectLayers.empty() || out.objectLayers.back().objects.empty() ||
                expectedProperties == std::numeric_limits<std::uint64_t>::max() ||
                readProperties >= expectedProperties || tokens.size() < 2U) return fail("bad property");
            const std::size_t valuePos = tokens.size() >= 3U ? line.find(tokens[2]) : std::string_view::npos;
            out.objectLayers.back().objects.back().properties.push_back(TileObjectProperty{
                std::string(tokens[1]), valuePos == std::string_view::npos ? std::string{} : std::string(line.substr(valuePos))});
            ++readProperties;
        } else {
            return fail("unknown dvetilemap key");
        }
    }
    if (version == 0U) return fail("empty or non-dvetilemap input");
    if (!out.layers.empty() && currentLayerRows != out.layers.back().height)
        return fail("layer row count mismatch");
    if (!finish_properties() || !finish_objects()) return fail("object declaration count mismatch");
    if (expectedTileCount != std::numeric_limits<std::uint64_t>::max() &&
        expectedTileCount != out.tileset.tiles.size()) return fail("tileset definition count mismatch");
    if (expectedRuleCount != std::numeric_limits<std::uint64_t>::max() &&
        expectedRuleCount != out.tileset.autotileRules.size()) return fail("autotile rule count mismatch");
    if (expectedLayerCount != std::numeric_limits<std::uint64_t>::max() &&
        expectedLayerCount != out.layers.size()) return fail("layer count mismatch");
    if (expectedObjectLayerCount != std::numeric_limits<std::uint64_t>::max() &&
        expectedObjectLayerCount != out.objectLayers.size()) return fail("object layer count mismatch");
    return out.validate(error);
}

std::uint64_t TileMap::content_hash() const {
    std::uint64_t state = 0xcbf29ce484222325ULL;
    hash_bytes(state, serialize());
    return state;
}

TileWorldQuery query_tile_world(const TileMap& map, const TileAabb& bounds) {
    TileWorldQuery result;
    if (!(bounds.size.x >= 0.0F && bounds.size.y >= 0.0F) || map.tileWidth == 0U || map.tileHeight == 0U)
        return result;
    const std::int64_t minCol = floor_div(bounds.min.x, map.tileWidth);
    const std::int64_t minRow = floor_div(bounds.min.y, map.tileHeight);
    const std::int64_t maxCol = floor_div(std::nextafter(bounds.max_x(), bounds.min.x), map.tileWidth);
    const std::int64_t maxRow = floor_div(std::nextafter(bounds.max_y(), bounds.min.y), map.tileHeight);
    for (std::size_t layerIndex = 0U; layerIndex < map.layers.size(); ++layerIndex) {
        const TileLayer& layer = map.layers[layerIndex];
        if (!layer.visible) continue;
        for (std::int64_t row = std::max<std::int64_t>(0, minRow);
             row <= maxRow && row < layer.height; ++row) {
            for (std::int64_t col = std::max<std::int64_t>(0, minCol);
                 col <= maxCol && col < layer.width; ++col) {
                const std::uint32_t value = layer.at(static_cast<std::uint32_t>(col),
                                                     static_cast<std::uint32_t>(row));
                const std::uint32_t id = tile_id(value);
                if (id == 0U || id > map.tileset.tiles.size()) continue;
                const TileDef& def = map.tileset.tiles[id - 1U];
                TileCellHit hit;
                hit.layerIndex = layerIndex;
                hit.col = static_cast<std::uint32_t>(col);
                hit.row = static_cast<std::uint32_t>(row);
                hit.tileValue = value;
                hit.collision = def.collision;
                hit.material = def.material;
                hit.damagePerSecond = def.damagePerSecond;
                hit.conveyorVelocity = def.conveyorVelocity;
                hit.ladder = def.ladder;
                hit.trigger = def.trigger;
                result.totalDamagePerSecond += def.damagePerSecond;
                result.conveyorVelocity.x += def.conveyorVelocity.x;
                result.conveyorVelocity.y += def.conveyorVelocity.y;
                result.onLadder = result.onLadder || def.ladder;
                if (!def.trigger.empty() &&
                    std::find(result.triggers.begin(), result.triggers.end(), def.trigger) == result.triggers.end()) {
                    result.triggers.push_back(def.trigger);
                }
                result.cells.push_back(std::move(hit));
            }
        }
    }
    for (std::size_t layerIndex = 0U; layerIndex < map.objectLayers.size(); ++layerIndex) {
        const TileObjectLayer& layer = map.objectLayers[layerIndex];
        if (!layer.visible) continue;
        for (const TileObject& object : layer.objects) {
            const float width = object.shape == TileObjectShape::Point ? 0.0F : object.size.x;
            const float height = object.shape == TileObjectShape::Point ? 0.0F : object.size.y;
            const bool overlaps = object.position.x <= bounds.max_x() && object.position.x + width >= bounds.min.x &&
                object.position.y <= bounds.max_y() && object.position.y + height >= bounds.min.y;
            if (!overlaps) continue;
            result.objects.push_back(TileObjectHit{layerIndex, object.id, object.type, object.name});
        }
    }
    return result;
}

std::vector<TileRenderItem> build_tile_render_list(const TileMap& map,
                                                   const TileAabb& worldView,
                                                   std::uint64_t animationTicks) {
    std::vector<TileRenderItem> items;
    if (map.tileWidth == 0U || map.tileHeight == 0U ||
        worldView.size.x <= 0.0F || worldView.size.y <= 0.0F) return items;
    const std::int64_t minCol = floor_div(worldView.min.x, map.tileWidth);
    const std::int64_t minRow = floor_div(worldView.min.y, map.tileHeight);
    const std::int64_t maxCol = floor_div(std::nextafter(worldView.max_x(), worldView.min.x), map.tileWidth);
    const std::int64_t maxRow = floor_div(std::nextafter(worldView.max_y(), worldView.min.y), map.tileHeight);
    for (std::size_t layerIndex = 0U; layerIndex < map.layers.size(); ++layerIndex) {
        const TileLayer& layer = map.layers[layerIndex];
        if (!layer.visible) continue;
        for (std::int64_t row = std::max<std::int64_t>(0, minRow);
             row <= maxRow && row < static_cast<std::int64_t>(layer.height); ++row) {
            for (std::int64_t col = std::max<std::int64_t>(0, minCol);
                 col <= maxCol && col < static_cast<std::int64_t>(layer.width); ++col) {
                const std::uint32_t value = layer.at(static_cast<std::uint32_t>(col),
                                                     static_cast<std::uint32_t>(row));
                if (tile_id(value) == 0U) continue;
                TileRenderItem item;
                item.layerIndex = layerIndex;
                item.col = static_cast<std::uint32_t>(col);
                item.row = static_cast<std::uint32_t>(row);
                item.tileValue = value;
                item.atlasIndex = map.tileset.atlas_index_for(value, animationTicks);
                item.worldBounds = {{static_cast<float>(col) * static_cast<float>(map.tileWidth),
                                     static_cast<float>(row) * static_cast<float>(map.tileHeight)},
                                    {static_cast<float>(map.tileWidth), static_cast<float>(map.tileHeight)}};
                item.parallaxX = layer.parallaxX;
                item.parallaxY = layer.parallaxY;
                item.flipX = tile_flip_x(value);
                item.flipY = tile_flip_y(value);
                items.push_back(std::move(item));
            }
        }
    }
    return items;
}

std::vector<TileChunkDiagnostic> build_tile_chunk_diagnostics(const TileMap& map,
                                                               std::uint32_t chunkWidth,
                                                               std::uint32_t chunkHeight) {
    std::vector<TileChunkDiagnostic> diagnostics;
    if (chunkWidth == 0U || chunkHeight == 0U) return diagnostics;
    for (std::size_t layerIndex = 0U; layerIndex < map.layers.size(); ++layerIndex) {
        const TileLayer& layer = map.layers[layerIndex];
        const std::uint32_t chunksX = layer.width / chunkWidth +
            (layer.width % chunkWidth == 0U ? 0U : 1U);
        const std::uint32_t chunksY = layer.height / chunkHeight +
            (layer.height % chunkHeight == 0U ? 0U : 1U);
        for (std::uint32_t chunkRow = 0U; chunkRow < chunksY; ++chunkRow) {
            for (std::uint32_t chunkCol = 0U; chunkCol < chunksX; ++chunkCol) {
                TileChunkDiagnostic diagnostic;
                diagnostic.layerIndex = layerIndex;
                diagnostic.chunkCol = chunkCol;
                diagnostic.chunkRow = chunkRow;
                diagnostic.contentHash = 0xcbf29ce484222325ULL;
                const std::uint32_t minCol = chunkCol * chunkWidth;
                const std::uint32_t minRow = chunkRow * chunkHeight;
                const std::uint32_t maxCol = std::min(layer.width, minCol + chunkWidth);
                const std::uint32_t maxRow = std::min(layer.height, minRow + chunkHeight);
                for (std::uint32_t row = minRow; row < maxRow; ++row) {
                    for (std::uint32_t col = minCol; col < maxCol; ++col) {
                        const std::uint32_t value = layer.at(col, row);
                        for (std::uint32_t byte = 0U; byte < 4U; ++byte) {
                            diagnostic.contentHash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU);
                            diagnostic.contentHash *= 0x100000001b3ULL;
                        }
                        const std::uint32_t id = tile_id(value);
                        if (id == 0U || id > map.tileset.tiles.size()) continue;
                        ++diagnostic.nonEmptyTiles;
                        const TileDef& def = map.tileset.tiles[id - 1U];
                        if (def.collision != TileCollision::Empty) ++diagnostic.collidableTiles;
                        if (def.damagePerSecond > 0.0F || layer.kind == TileLayerKind::Hazard)
                            ++diagnostic.hazardTiles;
                        if (!def.trigger.empty() || layer.kind == TileLayerKind::Trigger)
                            ++diagnostic.triggerTiles;
                    }
                }
                diagnostics.push_back(std::move(diagnostic));
            }
        }
    }
    for (std::size_t objectLayerIndex = 0U; objectLayerIndex < map.objectLayers.size();
         ++objectLayerIndex) {
        const TileObjectLayer& layer = map.objectLayers[objectLayerIndex];
        std::vector<TileChunkDiagnostic> objectChunks;
        for (const TileObject& object : layer.objects) {
            if (object.position.x < 0.0F || object.position.y < 0.0F) continue;
            const std::uint32_t col = static_cast<std::uint32_t>(object.position.x /
                static_cast<float>(map.tileWidth));
            const std::uint32_t row = static_cast<std::uint32_t>(object.position.y /
                static_cast<float>(map.tileHeight));
            const std::uint32_t chunkCol = col / chunkWidth;
            const std::uint32_t chunkRow = row / chunkHeight;
            auto found = std::find_if(objectChunks.begin(), objectChunks.end(),
                [&](const TileChunkDiagnostic& diagnostic) {
                    return diagnostic.chunkCol == chunkCol && diagnostic.chunkRow == chunkRow;
                });
            if (found == objectChunks.end()) {
                TileChunkDiagnostic diagnostic;
                diagnostic.layerIndex = map.layers.size() + objectLayerIndex;
                diagnostic.chunkCol = chunkCol;
                diagnostic.chunkRow = chunkRow;
                diagnostic.contentHash = 0xcbf29ce484222325ULL;
                objectChunks.push_back(diagnostic);
                found = std::prev(objectChunks.end());
            }
            ++found->objectCount;
            for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
                found->contentHash ^= static_cast<std::uint8_t>((object.id >> (byte * 8U)) & 0xFFU);
                found->contentHash *= 0x100000001b3ULL;
            }
        }
        std::sort(objectChunks.begin(), objectChunks.end(),
                  [](const TileChunkDiagnostic& left, const TileChunkDiagnostic& right) {
                      if (left.chunkRow != right.chunkRow) return left.chunkRow < right.chunkRow;
                      return left.chunkCol < right.chunkCol;
                  });
        diagnostics.insert(diagnostics.end(), objectChunks.begin(), objectChunks.end());
    }
    return diagnostics;
}

// ---------------------------------------------------------------------------------------------
// Collision grid + swept AABB
// ---------------------------------------------------------------------------------------------

TileCollision CollisionGrid::at(std::int64_t col, std::int64_t row) const noexcept {
    if (col < 0 || row < 0 || col >= static_cast<std::int64_t>(width) || row >= static_cast<std::int64_t>(height)) {
        return TileCollision::Empty;
    }
    return cells[static_cast<std::size_t>(row) * width + static_cast<std::size_t>(col)];
}

void CollisionGrid::set(std::uint32_t col, std::uint32_t row, TileCollision collision) noexcept {
    if (col >= width || row >= height) return;
    cells[static_cast<std::size_t>(row) * width + col] = collision;
}

namespace {
[[nodiscard]] int collision_priority(TileCollision collision) noexcept {
    switch (collision) {
    case TileCollision::Empty: return 0;
    case TileCollision::OneWayTop: return 1;
    case TileCollision::SlopeUpRight:
    case TileCollision::SlopeUpLeft: return 2;
    case TileCollision::Solid: return 3;
    }
    return 0;
}

[[nodiscard]] TileCollision collision_at_map_cell(const TileMap& map, std::uint32_t col,
                                                   std::uint32_t row) noexcept {
    TileCollision result = TileCollision::Empty;
    for (const auto& layer : map.layers) {
        if (!layer.collidable || col >= layer.width || row >= layer.height) continue;
        const TileCollision candidate = map.tileset.collision_for(layer.at(col, row));
        if (collision_priority(candidate) > collision_priority(result)) result = candidate;
    }
    return result;
}
} // namespace

CollisionGrid build_collision_grid(const TileMap& map) {
    CollisionGrid grid;
    grid.tileWidth = map.tileWidth;
    grid.tileHeight = map.tileHeight;
    for (const auto& layer : map.layers) {
        if (!layer.collidable) continue;
        grid.width = std::max(grid.width, layer.width);
        grid.height = std::max(grid.height, layer.height);
    }
    grid.cells.assign(static_cast<std::size_t>(grid.width) * grid.height, TileCollision::Empty);
    for (std::uint32_t row = 0; row < grid.height; ++row) {
        for (std::uint32_t col = 0; col < grid.width; ++col) {
            grid.set(col, row, collision_at_map_cell(map, col, row));
        }
    }
    return grid;
}

bool update_collision_grid_region(CollisionGrid& grid, const TileMap& map,
                                  std::uint32_t minCol, std::uint32_t minRow,
                                  std::uint32_t maxCol, std::uint32_t maxRow) noexcept {
    if (grid.tileWidth != map.tileWidth || grid.tileHeight != map.tileHeight ||
        minCol > maxCol || minRow > maxRow || maxCol >= grid.width || maxRow >= grid.height) {
        return false;
    }
    for (std::uint32_t row = minRow; row <= maxRow; ++row) {
        for (std::uint32_t col = minCol; col <= maxCol; ++col) {
            grid.set(col, row, collision_at_map_cell(map, col, row));
        }
    }
    return true;
}

AabbSweep move_aabb(const CollisionGrid& grid, const TileAabb& box, TileVec2 displacement,
                    AabbMoveOptions options) noexcept {
    AabbSweep result;
    result.velocity = displacement;

    const float tw = static_cast<float>(grid.tileWidth);
    const float th = static_cast<float>(grid.tileHeight);
    const float epsilon = 0.001F;

    float minX = box.min.x;
    float minY = box.min.y;
    const float sizeX = box.size.x;
    const float sizeY = box.size.y;

    // Solid tiles block horizontal movement. Slopes remain horizontally traversable and are
    // resolved as floor surfaces after the horizontal step.
    if (displacement.x != 0.0F) {
        const float newMinX = minX + displacement.x;
        const std::int64_t rowStart = floor_div(minY + epsilon, grid.tileHeight);
        const std::int64_t rowEnd = floor_div(minY + sizeY - epsilon, grid.tileHeight);
        if (displacement.x > 0.0F) {
            const float previousRight = minX + sizeX;
            const std::int64_t startCol = floor_div(previousRight - epsilon, grid.tileWidth);
            const std::int64_t endCol = floor_div(newMinX + sizeX - epsilon, grid.tileWidth);
            for (std::int64_t col = startCol; col <= endCol && !result.hitRight; ++col) {
                const float tileLeft = static_cast<float>(col) * tw;
                if (tileLeft < previousRight - epsilon) continue;
                for (std::int64_t row = rowStart; row <= rowEnd; ++row) {
                    if (grid.at(col, row) == TileCollision::Solid) {
                        minX = tileLeft - sizeX;
                        result.hitRight = true;
                        result.velocity.x = 0.0F;
                        break;
                    }
                }
            }
            if (!result.hitRight) minX = newMinX;
        } else {
            const float previousLeft = minX;
            const std::int64_t startCol = floor_div(previousLeft + epsilon, grid.tileWidth);
            const std::int64_t endCol = floor_div(newMinX + epsilon, grid.tileWidth);
            for (std::int64_t col = startCol; col >= endCol && !result.hitLeft; --col) {
                const float tileRight = static_cast<float>(col + 1) * tw;
                if (tileRight > previousLeft + epsilon) continue;
                for (std::int64_t row = rowStart; row <= rowEnd; ++row) {
                    if (grid.at(col, row) == TileCollision::Solid) {
                        minX = tileRight;
                        result.hitLeft = true;
                        result.velocity.x = 0.0F;
                        break;
                    }
                }
            }
            if (!result.hitLeft) minX = newMinX;
        }
    }

    if (displacement.y != 0.0F) {
        const float newMinY = minY + displacement.y;
        const std::int64_t colStart = floor_div(minX + epsilon, grid.tileWidth);
        const std::int64_t colEnd = floor_div(minX + sizeX - epsilon, grid.tileWidth);
        if (displacement.y > 0.0F) {
            const float previousBottom = minY + sizeY;
            const std::int64_t startRow = floor_div(previousBottom - epsilon, grid.tileHeight);
            const std::int64_t endRow = floor_div(newMinY + sizeY - epsilon, grid.tileHeight);
            for (std::int64_t row = startRow; row <= endRow && !result.hitBottom; ++row) {
                const float tileTop = static_cast<float>(row) * th;
                if (tileTop < previousBottom - epsilon) continue;
                for (std::int64_t col = colStart; col <= colEnd; ++col) {
                    const TileCollision c = grid.at(col, row);
                    bool blocks = c == TileCollision::Solid;
                    if (c == TileCollision::OneWayTop && !options.ignoreOneWayPlatforms &&
                        previousBottom <= tileTop + epsilon) blocks = true;
                    if (blocks) {
                        minY = tileTop - sizeY;
                        result.hitBottom = true;
                        result.onGround = true;
                        result.groundNormal = {0.0F, -1.0F};
                        result.velocity.y = 0.0F;
                        break;
                    }
                }
            }
            if (!result.hitBottom) minY = newMinY;
        } else {
            const float previousTop = minY;
            const std::int64_t startRow = floor_div(previousTop + epsilon, grid.tileHeight);
            const std::int64_t endRow = floor_div(newMinY + epsilon, grid.tileHeight);
            for (std::int64_t row = startRow; row >= endRow && !result.hitTop; --row) {
                const float tileBottom = static_cast<float>(row + 1) * th;
                if (tileBottom > previousTop + epsilon) continue;
                for (std::int64_t col = colStart; col <= colEnd; ++col) {
                    if (grid.at(col, row) == TileCollision::Solid) {
                        minY = tileBottom;
                        result.hitTop = true;
                        result.velocity.y = 0.0F;
                        break;
                    }
                }
            }
            if (!result.hitTop) minY = newMinY;
        }
    }

    // Resolve authored floor slopes at the mover's bottom-center. A slope only catches a mover
    // whose previous bottom was at or above the sampled surface, preventing side/below snagging.
    if (displacement.y >= 0.0F && grid.width > 0 && grid.height > 0) {
        const float footX = minX + sizeX * 0.5F;
        const float proposedBottom = minY + sizeY;
        const float previousBottom = box.min.y + sizeY;
        const std::int64_t col = floor_div(footX, grid.tileWidth);
        const std::int64_t rowStart = floor_div(previousBottom - epsilon, grid.tileHeight);
        const std::int64_t rowEnd = floor_div(proposedBottom + epsilon, grid.tileHeight);
        for (std::int64_t row = rowStart; row <= rowEnd; ++row) {
            const TileCollision c = grid.at(col, row);
            if (c != TileCollision::SlopeUpRight && c != TileCollision::SlopeUpLeft) continue;
            const float tileLeft = static_cast<float>(col) * tw;
            const float tileTop = static_cast<float>(row) * th;
            const float local = std::clamp((footX - tileLeft) / tw, 0.0F, 1.0F);
            const float surface = c == TileCollision::SlopeUpRight
                ? tileTop + th * (1.0F - local)
                : tileTop + th * local;
            if (previousBottom <= surface + epsilon && proposedBottom >= surface - epsilon) {
                minY = surface - sizeY;
                result.hitBottom = true;
                result.onGround = true;
                const float nx = c == TileCollision::SlopeUpRight ? -th : th;
                const float ny = -tw;
                const float invLength = 1.0F / std::sqrt(nx * nx + ny * ny);
                result.groundNormal = {nx * invLength, ny * invLength};
                result.velocity.y = 0.0F;
                break;
            }
        }
    }

    result.position = {minX, minY};
    return result;
}

// ---------------------------------------------------------------------------------------------
// Camera and parallax
// ---------------------------------------------------------------------------------------------

TileAabb Camera2D::view_bounds() const noexcept {
    return TileAabb{{center.x - viewWidth * 0.5F, center.y - viewHeight * 0.5F}, {viewWidth, viewHeight}};
}

void camera_follow(Camera2D& camera, TileVec2 target) noexcept {
    // Nudge the center only enough to keep the target inside the dead zone.
    const float dx = target.x - camera.center.x;
    if (dx > camera.deadZoneHalf.x) camera.center.x += dx - camera.deadZoneHalf.x;
    else if (dx < -camera.deadZoneHalf.x) camera.center.x += dx + camera.deadZoneHalf.x;

    const float dy = target.y - camera.center.y;
    if (dy > camera.deadZoneHalf.y) camera.center.y += dy - camera.deadZoneHalf.y;
    else if (dy < -camera.deadZoneHalf.y) camera.center.y += dy + camera.deadZoneHalf.y;

    // Clamp the view to the world bounds if they form a valid rectangle.
    if (camera.worldMax.x > camera.worldMin.x && camera.worldMax.y > camera.worldMin.y) {
        const float halfW = camera.viewWidth * 0.5F;
        const float halfH = camera.viewHeight * 0.5F;
        const float worldW = camera.worldMax.x - camera.worldMin.x;
        const float worldH = camera.worldMax.y - camera.worldMin.y;
        if (worldW >= camera.viewWidth) {
            camera.center.x = std::clamp(camera.center.x, camera.worldMin.x + halfW, camera.worldMax.x - halfW);
        } else {
            camera.center.x = camera.worldMin.x + worldW * 0.5F; // level narrower than view: center it
        }
        if (worldH >= camera.viewHeight) {
            camera.center.y = std::clamp(camera.center.y, camera.worldMin.y + halfH, camera.worldMax.y - halfH);
        } else {
            camera.center.y = camera.worldMin.y + worldH * 0.5F;
        }
    }
}

void camera_set_world_from_map(Camera2D& camera, const TileMap& map) noexcept {
    camera.worldMin = {0.0F, 0.0F};
    camera.worldMax = {static_cast<float>(map.pixel_width()), static_cast<float>(map.pixel_height())};
}

TileVec2 parallax_layer_origin(const Camera2D& camera, float parallaxX, float parallaxY) noexcept {
    // The layer's top-left in screen space: the camera's top-left scaled by the parallax factor.
    const float camLeft = camera.center.x - camera.viewWidth * 0.5F;
    const float camTop = camera.center.y - camera.viewHeight * 0.5F;
    return {camLeft * parallaxX, camTop * parallaxY};
}

} // namespace dve
