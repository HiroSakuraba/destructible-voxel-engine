#include "dve/sprite_diagnostics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace dve {
namespace {

constexpr float kSnapTolerancePixels = 0.001F;
constexpr std::uint64_t kMaximumExactAtlasPixels = 32ULL * 1024ULL * 1024ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

template<class Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t shift = 0U; shift < sizeof(Unsigned) * 8U; shift += 8U)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & static_cast<Unsigned>(0xFFU)));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, value.size());
    for (const char character : value) hash_byte(hash, static_cast<std::uint8_t>(character));
}

[[nodiscard]] bool contains(std::span<const std::string> values, std::string_view wanted) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return value == wanted;
    });
}

[[nodiscard]] std::array<float, 2> project_vertex(
    const SpriteVertex& vertex, const SpriteDiagnosticsInput& input) noexcept {
    const float width = static_cast<float>(input.logicalWidth);
    const float height = static_cast<float>(input.logicalHeight);
    const float pixelsPerUnit = input.presentation.pixelsPerWorldUnit;
    const float horizontal = (vertex.position.x - input.cameraOrigin.x) * pixelsPerUnit;
    const float verticalWorld = vertex.position.z - input.cameraOrigin.z;
    const float vertical = (input.plane == GameplayPlane2D::XY
        ? vertex.position.y - input.cameraOrigin.y : verticalWorld) * pixelsPerUnit;
    return {width * 0.5F + horizontal, height * 0.5F - vertical};
}

[[nodiscard]] float edge(float ax, float ay, float bx, float by, float px, float py) noexcept {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void increment_sample(SpriteOverdrawImage& image, int x, int y) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height)) return;
    const std::size_t index = static_cast<std::size_t>(y) * image.width +
        static_cast<std::size_t>(x);
    if (image.samples[index] != std::numeric_limits<std::uint16_t>::max())
        ++image.samples[index];
}

void raster_triangle(SpriteOverdrawImage& image,
                     const std::array<float, 2>& a,
                     const std::array<float, 2>& b,
                     const std::array<float, 2>& c) noexcept {
    const float area = edge(a[0], a[1], b[0], b[1], c[0], c[1]);
    if (std::fabs(area) <= 1.0e-8F) return;
    const int minimumX = std::max(0, static_cast<int>(std::floor(
        std::min({a[0], b[0], c[0]}))));
    const int maximumX = std::min(static_cast<int>(image.width) - 1,
        static_cast<int>(std::ceil(std::max({a[0], b[0], c[0]}))));
    const int minimumY = std::max(0, static_cast<int>(std::floor(
        std::min({a[1], b[1], c[1]}))));
    const int maximumY = std::min(static_cast<int>(image.height) - 1,
        static_cast<int>(std::ceil(std::max({a[1], b[1], c[1]}))));
    const bool positive = area > 0.0F;
    for (int y = minimumY; y <= maximumY; ++y) {
        for (int x = minimumX; x <= maximumX; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float ab = edge(a[0], a[1], b[0], b[1], px, py);
            const float bc = edge(b[0], b[1], c[0], c[1], px, py);
            const float ca = edge(c[0], c[1], a[0], a[1], px, py);
            if ((positive && ab >= 0.0F && bc >= 0.0F && ca >= 0.0F) ||
                (!positive && ab <= 0.0F && bc <= 0.0F && ca <= 0.0F))
                increment_sample(image, x, y);
        }
    }
}

void raster_tile(SpriteOverdrawImage& image, const TileRenderItem& tile,
                 const SpriteDiagnosticsInput& input) noexcept {
    const float left = static_cast<float>(input.logicalWidth) * 0.5F +
        tile.worldBounds.min.x - input.tileCameraCenterPixels.x;
    const float right = static_cast<float>(input.logicalWidth) * 0.5F +
        tile.worldBounds.max_x() - input.tileCameraCenterPixels.x;
    const float top = static_cast<float>(input.logicalHeight) * 0.5F -
        (tile.worldBounds.max_y() - input.tileCameraCenterPixels.y);
    const float bottom = static_cast<float>(input.logicalHeight) * 0.5F -
        (tile.worldBounds.min.y - input.tileCameraCenterPixels.y);
    const int minimumX = std::max(0, static_cast<int>(std::floor(std::min(left, right))));
    const int maximumX = std::min(static_cast<int>(image.width) - 1,
        static_cast<int>(std::ceil(std::max(left, right))) - 1);
    const int minimumY = std::max(0, static_cast<int>(std::floor(std::min(top, bottom))));
    const int maximumY = std::min(static_cast<int>(image.height) - 1,
        static_cast<int>(std::ceil(std::max(top, bottom))) - 1);
    for (int y = minimumY; y <= maximumY; ++y)
        for (int x = minimumX; x <= maximumX; ++x) increment_sample(image, x, y);
}

[[nodiscard]] std::array<std::uint8_t, 4> heat_color(std::uint16_t value,
                                                     std::uint16_t maximum) noexcept {
    if (value == 0U) return {0U, 0U, 0U, 0U};
    const float normalized = maximum > 1U
        ? static_cast<float>(value - 1U) / static_cast<float>(maximum - 1U) : 0.0F;
    const float red = std::clamp(normalized * 2.0F, 0.0F, 1.0F);
    const float green = std::clamp(2.0F - std::fabs(normalized * 4.0F - 2.0F), 0.0F, 1.0F);
    const float blue = std::clamp(1.0F - normalized * 2.0F, 0.0F, 1.0F);
    return {static_cast<std::uint8_t>(std::lround(red * 255.0F)),
            static_cast<std::uint8_t>(std::lround(green * 255.0F)),
            static_cast<std::uint8_t>(std::lround(blue * 255.0F)), 220U};
}

[[nodiscard]] SpriteBatchBreakReason batch_break_reason(
    const SpriteDrawItem& previous, const SpriteDrawItem& current) noexcept {
    SpriteBatchBreakReason reasons = SpriteBatchBreakReason::NoBreak;
    if (previous.textureAsset != current.textureAsset) reasons = reasons | SpriteBatchBreakReason::Texture;
    if (previous.materialId != current.materialId) reasons = reasons | SpriteBatchBreakReason::Material;
    if (previous.paletteBank != current.paletteBank) reasons = reasons | SpriteBatchBreakReason::PaletteBank;
    if (previous.paletteAsset != current.paletteAsset) reasons = reasons | SpriteBatchBreakReason::PaletteAsset;
    if (previous.paletteStateHash != current.paletteStateHash ||
        previous.palettePacket != current.palettePacket)
        reasons = reasons | SpriteBatchBreakReason::PaletteState;
    if (previous.sampling != current.sampling) reasons = reasons | SpriteBatchBreakReason::Sampling;
    if (previous.blendMode != current.blendMode) reasons = reasons | SpriteBatchBreakReason::BlendMode;
    return reasons;
}

[[nodiscard]] std::string batch_break_summary(SpriteBatchBreakReason reasons) {
    std::vector<std::string_view> names;
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::Texture)) names.push_back("texture");
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::Material)) names.push_back("material");
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::PaletteBank)) names.push_back("palette bank");
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::PaletteAsset)) names.push_back("palette asset");
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::PaletteState)) names.push_back("palette state");
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::Sampling)) names.push_back("sampling");
    if (has_batch_break_reason(reasons, SpriteBatchBreakReason::BlendMode)) names.push_back("blend mode");
    std::string result;
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (index != 0U) result += ", ";
        result += names[index];
    }
    return result;
}

[[nodiscard]] SpriteAtlasDiagnostic analyze_atlas(const SpriteAsset& asset) {
    SpriteAtlasDiagnostic result;
    result.assetName = asset.name;
    result.textureAsset = asset.textureAsset;
    result.textureWidth = asset.textureWidth;
    result.textureHeight = asset.textureHeight;
    result.frameCount = asset.frames.size();
    result.atlasPixels = static_cast<std::uint64_t>(asset.textureWidth) * asset.textureHeight;
    result.freePixels = result.atlasPixels;
    if (result.atlasPixels == 0U || asset.frames.empty()) return result;

    std::uint32_t minimumX = asset.textureWidth;
    std::uint32_t minimumY = asset.textureHeight;
    std::uint32_t maximumX = 0U;
    std::uint32_t maximumY = 0U;
    for (const SpriteFrame& frame : asset.frames) {
        minimumX = std::min(minimumX, frame.atlasRect.x);
        minimumY = std::min(minimumY, frame.atlasRect.y);
        maximumX = std::max(maximumX, frame.atlasRect.x + frame.atlasRect.width);
        maximumY = std::max(maximumY, frame.atlasRect.y + frame.atlasRect.height);
    }
    if (maximumX > minimumX && maximumY > minimumY)
        result.boundingPixels = static_cast<std::uint64_t>(maximumX - minimumX) *
            static_cast<std::uint64_t>(maximumY - minimumY);

    if (result.atlasPixels <= kMaximumExactAtlasPixels) {
        result.exact = true;
        std::vector<std::uint8_t> occupied(static_cast<std::size_t>(result.atlasPixels));
        for (const SpriteFrame& frame : asset.frames) {
            const std::uint32_t endX = std::min(asset.textureWidth,
                frame.atlasRect.x + frame.atlasRect.width);
            const std::uint32_t endY = std::min(asset.textureHeight,
                frame.atlasRect.y + frame.atlasRect.height);
            for (std::uint32_t y = frame.atlasRect.y; y < endY; ++y) {
                for (std::uint32_t x = frame.atlasRect.x; x < endX; ++x) {
                    std::uint8_t& value = occupied[static_cast<std::size_t>(y) * asset.textureWidth + x];
                    if (value == 0U) ++result.occupiedPixels;
                    else ++result.overlappingPixels;
                    if (value != std::numeric_limits<std::uint8_t>::max()) ++value;
                }
            }
        }
    } else {
        result.exact = false;
        for (const SpriteFrame& frame : asset.frames) {
            const std::uint32_t endX = std::min(asset.textureWidth,
                frame.atlasRect.x + frame.atlasRect.width);
            const std::uint32_t endY = std::min(asset.textureHeight,
                frame.atlasRect.y + frame.atlasRect.height);
            if (endX > frame.atlasRect.x && endY > frame.atlasRect.y)
                result.occupiedPixels += static_cast<std::uint64_t>(endX - frame.atlasRect.x) *
                    static_cast<std::uint64_t>(endY - frame.atlasRect.y);
        }
        result.occupiedPixels = std::min(result.occupiedPixels, result.atlasPixels);
    }
    result.freePixels = result.atlasPixels - std::min(result.atlasPixels, result.occupiedPixels);
    result.occupancy = static_cast<double>(result.occupiedPixels) /
        static_cast<double>(result.atlasPixels);
    if (result.boundingPixels != 0U)
        result.packingFragmentation = 1.0 - static_cast<double>(result.occupiedPixels) /
            static_cast<double>(result.boundingPixels);
    result.packingFragmentation = std::clamp(result.packingFragmentation, 0.0, 1.0);
    return result;
}

[[nodiscard]] const SpriteAsset* find_asset(
    std::span<const SpriteAssetCatalogEntry> assets, SpriteAssetId id) noexcept {
    const auto found = std::find_if(assets.begin(), assets.end(), [id](const auto& entry) {
        return entry.id == id;
    });
    return found == assets.end() ? nullptr : found->asset;
}

void add_reference_issue(std::vector<SpriteReferenceIssue>& output,
                         SpriteReferenceKind kind, SpriteOwnerId owner, SpriteAssetId asset,
                         std::string reference, std::string location, std::string message,
                         SpriteDiagnosticSeverity severity = SpriteDiagnosticSeverity::Error) {
    output.push_back({severity, kind, owner, asset, std::move(reference),
                      std::move(location), std::move(message)});
}

void audit_references(const SpriteDiagnosticsInput& input,
                      std::vector<SpriteReferenceIssue>& output) {
    if (input.sprites != nullptr) {
        for (const SpriteDrawItem& item : input.sprites->items) {
            if (item.textureAsset.empty() || !contains(input.residentTextures, item.textureAsset))
                add_reference_issue(output, SpriteReferenceKind::Texture, item.owner, item.asset,
                    item.textureAsset, "sprite draw item",
                    item.textureAsset.empty() ? "Draw item has no texture reference"
                                              : "Draw-item texture is not resident or registered");
            if (!item.paletteAsset.empty() &&
                !contains(input.residentPalettes, item.paletteAsset))
                add_reference_issue(output, SpriteReferenceKind::Palette, item.owner, item.asset,
                    item.paletteAsset, "sprite draw item",
                    "Draw-item palette is not resident or registered");
        }
    }
    for (const SpriteAssetCatalogEntry& entry : input.assets) {
        if (entry.asset == nullptr) continue;
        const SpriteAsset& asset = *entry.asset;
        if (asset.textureAsset.empty() || !contains(input.residentTextures, asset.textureAsset))
            add_reference_issue(output, SpriteReferenceKind::Texture, kInvalidSpriteOwnerId,
                entry.id, asset.textureAsset, "sprite asset " + asset.name,
                asset.textureAsset.empty() ? "Sprite asset has no texture reference"
                                           : "Texture is not resident or registered");
        if (!asset.paletteAsset.empty() && !contains(input.residentPalettes, asset.paletteAsset))
            add_reference_issue(output, SpriteReferenceKind::Palette, kInvalidSpriteOwnerId,
                entry.id, asset.paletteAsset, "sprite asset " + asset.name,
                "Palette is not resident or registered");
    }

    for (const SpriteInstanceReference& instance : input.instances) {
        const SpriteAsset* asset = find_asset(input.assets, instance.asset);
        if (asset == nullptr) {
            add_reference_issue(output, SpriteReferenceKind::Clip, instance.owner, instance.asset,
                instance.clip, "sprite instance", "Sprite asset is missing");
            continue;
        }
        if (find_sprite_clip(*asset, instance.clip) == nullptr)
            add_reference_issue(output, SpriteReferenceKind::Clip, instance.owner, instance.asset,
                instance.clip, "sprite instance", "Referenced animation clip does not exist");
    }

    for (const SpriteSocketReference& socket : input.socketReferences) {
        const SpriteAsset* asset = find_asset(input.assets, socket.asset);
        if (asset == nullptr) {
            add_reference_issue(output, SpriteReferenceKind::Socket, socket.owner, socket.asset,
                socket.socket, "socket attachment", "Sprite asset is missing");
            continue;
        }
        const SpriteClip* clip = find_sprite_clip(*asset, socket.clip);
        if (clip == nullptr) {
            add_reference_issue(output, SpriteReferenceKind::Clip, socket.owner, socket.asset,
                socket.clip, "socket attachment", "Socket clip does not exist");
            continue;
        }
        const bool exists = std::any_of(clip->socketKeys.begin(), clip->socketKeys.end(),
            [&](const SpriteSocketKey& key) { return key.name == socket.socket; });
        if (!exists)
            add_reference_issue(output, SpriteReferenceKind::Socket, socket.owner, socket.asset,
                socket.socket, "clip " + socket.clip, "Referenced socket does not exist in the clip");
    }

    for (const SpriteMachineReference& machineReference : input.machines) {
        if (machineReference.machine == nullptr) continue;
        const SpriteAnimationStateMachineAsset& machine = *machineReference.machine;
        const SpriteAsset* sprite = find_asset(input.assets, machineReference.spriteAsset);
        std::unordered_set<std::string> states;
        std::unordered_set<std::string> parameters;
        if (sprite == nullptr)
            add_reference_issue(output, SpriteReferenceKind::State, kInvalidSpriteOwnerId,
                machineReference.spriteAsset, machine.name, "machine " + machineReference.name,
                "Compatible sprite asset is missing");
        for (const SpriteAnimationParameterDefinition& parameter : machine.parameters)
            parameters.insert(parameter.name);
        for (const SpriteAnimationState& state : machine.states) {
            states.insert(state.name);
            if (sprite != nullptr && find_sprite_clip(*sprite, state.clip) == nullptr)
                add_reference_issue(output, SpriteReferenceKind::Clip, kInvalidSpriteOwnerId,
                    machineReference.spriteAsset, state.clip,
                    "machine " + machineReference.name + " state " + state.name,
                    "State references a missing clip");
        }
        if (!machine.initialState.empty() && !states.contains(machine.initialState))
            add_reference_issue(output, SpriteReferenceKind::State, kInvalidSpriteOwnerId,
                machineReference.spriteAsset, machine.initialState,
                "machine " + machineReference.name, "Initial state does not exist");
        for (std::size_t index = 0U; index < machine.transitions.size(); ++index) {
            const SpriteAnimationTransition& transition = machine.transitions[index];
            const std::string location = "machine " + machineReference.name + " transition " +
                std::to_string(index);
            if (transition.fromState != "*" && !states.contains(transition.fromState))
                add_reference_issue(output, SpriteReferenceKind::State, kInvalidSpriteOwnerId,
                    machineReference.spriteAsset, transition.fromState, location,
                    "Transition source state does not exist");
            if (!states.contains(transition.toState))
                add_reference_issue(output, SpriteReferenceKind::State, kInvalidSpriteOwnerId,
                    machineReference.spriteAsset, transition.toState, location,
                    "Transition destination state does not exist");
            for (const SpriteAnimationCondition& condition : transition.conditions) {
                if (!parameters.contains(condition.parameter))
                    add_reference_issue(output, SpriteReferenceKind::Parameter,
                        kInvalidSpriteOwnerId, machineReference.spriteAsset,
                        condition.parameter, location,
                        "Transition condition references a missing parameter");
            }
        }
    }
}

[[nodiscard]] std::array<std::uint8_t, 4> sorting_color(std::int32_t layer) noexcept {
    std::uint32_t value = static_cast<std::uint32_t>(layer) * 2654435761U + 0x9E3779B9U;
    return {static_cast<std::uint8_t>(64U + (value & 127U)),
            static_cast<std::uint8_t>(64U + ((value >> 8U) & 127U)),
            static_cast<std::uint8_t>(64U + ((value >> 16U) & 127U)), 210U};
}

void append_budget_violation(std::vector<SpriteBudgetViolation>& output,
                             std::string metric, std::uint64_t measured,
                             std::uint64_t limit) {
    if (limit == 0U || measured <= limit) return;
    output.push_back({SpriteDiagnosticSeverity::Warning, metric, measured, limit,
        metric + " exceeds the active camera/profile budget by " +
        std::to_string(measured - limit)});
}

} // namespace

SpriteCameraBudget sprite_camera_budget_for_profile(
    SpriteBudgetProfile profile, std::uint32_t logicalWidth,
    std::uint32_t logicalHeight) noexcept {
    const std::uint64_t pixels = static_cast<std::uint64_t>(logicalWidth) * logicalHeight;
    switch (profile) {
        case SpriteBudgetProfile::Unrestricted: return {};
        case SpriteBudgetProfile::Retro8Bit:
            return {64U, 1024U, 32U, 32U, pixels, pixels * 2U, 8U, 8U, 4U};
        case SpriteBudgetProfile::Retro16Bit:
            return {256U, 4096U, 96U, 96U, pixels, pixels * 3U, 32U, 16U, 6U};
        case SpriteBudgetProfile::Handheld:
            return {512U, 8192U, 128U, 128U, pixels, pixels * 4U, 48U, 24U, 8U};
        case SpriteBudgetProfile::Mobile:
            return {4000U, 65536U, 512U, 512U, pixels, pixels * 6U, 256U, 64U, 12U};
        case SpriteBudgetProfile::Desktop:
            return {16000U, 262144U, 2048U, 2048U, pixels, pixels * 10U, 1024U, 256U, 24U};
    }
    return {};
}

SpriteOverdrawImage render_sprite_overdraw_reference(const SpriteDiagnosticsInput& input) {
    SpriteOverdrawImage result;
    result.width = input.logicalWidth;
    result.height = input.logicalHeight;
    if (result.width == 0U || result.height == 0U) return result;
    const std::uint64_t pixels = static_cast<std::uint64_t>(result.width) * result.height;
    if (pixels > std::numeric_limits<std::size_t>::max()) return {};
    result.samples.resize(static_cast<std::size_t>(pixels));
    if (input.sprites != nullptr) {
        for (const SpriteDrawItem& item : input.sprites->items) {
            std::array<std::array<float, 2>, 4> points{};
            for (std::size_t vertex = 0U; vertex < points.size(); ++vertex)
                points[vertex] = project_vertex(item.vertices[vertex], input);
            raster_triangle(result, points[0], points[1], points[2]);
            raster_triangle(result, points[0], points[2], points[3]);
        }
    }
    if (input.includeTilesInOverdraw)
        for (const TileRenderItem& tile : input.tiles) raster_tile(result, tile, input);

    for (const std::uint16_t value : result.samples) {
        if (value == 0U) continue;
        ++result.coveredPixels;
        result.fragmentCount += value;
        result.maximumOverdraw = std::max(result.maximumOverdraw, value);
    }
    if (result.coveredPixels != 0U)
        result.averageOverdrawOnCoveredPixels = static_cast<double>(result.fragmentCount) /
            static_cast<double>(result.coveredPixels);
    result.heatmapRgba8.resize(static_cast<std::size_t>(pixels) * 4U);
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0U; index < result.samples.size(); ++index) {
        const auto color = heat_color(result.samples[index], result.maximumOverdraw);
        const std::size_t offset = index * 4U;
        for (std::size_t component = 0U; component < 4U; ++component) {
            result.heatmapRgba8[offset + component] = static_cast<std::byte>(color[component]);
            hash_byte(hash, color[component]);
        }
    }
    result.contentHash = hash;
    return result;
}

SpriteDiagnosticsReport build_sprite_diagnostics(const SpriteDiagnosticsInput& input) {
    SpriteDiagnosticsReport result;
    result.cameraName = input.cameraName;
    result.profile = input.profile;
    result.budget = input.budget.value_or(sprite_camera_budget_for_profile(
        input.profile, input.logicalWidth, input.logicalHeight));
    result.tileCount = input.tiles.size();
    if (input.sprites != nullptr) {
        result.spriteCount = input.sprites->items.size();
        result.batchCount = input.sprites->batches.size();
        result.drawCallCount = result.batchCount;
        for (std::size_t index = 1U; index < input.sprites->items.size(); ++index) {
            const SpriteDrawItem& previous = input.sprites->items[index - 1U];
            const SpriteDrawItem& current = input.sprites->items[index];
            const SpriteBatchBreakReason reasons = batch_break_reason(previous, current);
            if (reasons == SpriteBatchBreakReason::NoBreak) continue;
            result.batchBreaks.push_back({index, previous.owner, current.owner, reasons,
                                           batch_break_summary(reasons)});
        }

        std::map<std::int32_t, SpriteSortingLayerDiagnostic> layers;
        std::map<std::tuple<std::int32_t, std::int32_t, float>, std::size_t> orderCounts;
        std::set<std::string, std::less<>> textures;
        std::set<std::string, std::less<>> palettes;
        for (const SpriteDrawItem& item : input.sprites->items) {
            textures.insert(item.textureAsset);
            if (!item.paletteAsset.empty()) palettes.insert(item.paletteAsset);
            auto [iterator, inserted] = layers.try_emplace(item.sortingLayer);
            SpriteSortingLayerDiagnostic& layer = iterator->second;
            if (inserted) {
                layer.sortingLayer = item.sortingLayer;
                layer.minimumOrder = layer.maximumOrder = item.orderInLayer;
                layer.minimumDepth = layer.maximumDepth = item.sortDepth;
                layer.displayColor = sorting_color(item.sortingLayer);
            } else {
                layer.minimumOrder = std::min(layer.minimumOrder, item.orderInLayer);
                layer.maximumOrder = std::max(layer.maximumOrder, item.orderInLayer);
                layer.minimumDepth = std::min(layer.minimumDepth, item.sortDepth);
                layer.maximumDepth = std::max(layer.maximumDepth, item.sortDepth);
            }
            layer.owners.push_back(item.owner);
            if (++orderCounts[{item.sortingLayer, item.orderInLayer, item.sortDepth}] > 1U)
                layer.ambiguousOrdering = true;

            const float ppu = input.presentation.pixelsPerWorldUnit;
            for (std::size_t vertex = 0U; vertex < item.vertices.size(); ++vertex) {
                const Float3 position = item.vertices[vertex].position;
                const float x = (position.x - input.cameraOrigin.x) * ppu;
                const float y = (input.plane == GameplayPlane2D::XY
                    ? position.y - input.cameraOrigin.y : position.z - input.cameraOrigin.z) * ppu;
                for (const auto& [axis, projected] : {std::pair{'x', x}, std::pair{'y', y}}) {
                    const float distance = std::fabs(projected - std::round(projected));
                    if (distance > kSnapTolerancePixels)
                        result.pixelSnapViolations.push_back({item.owner, vertex, axis,
                                                              projected, distance});
                }
            }
        }
        for (auto& [layerId, layer] : layers) {
            (void)layerId;
            result.sortingLayers.push_back(std::move(layer));
        }
        result.textureCount = textures.size();
        result.paletteCount = palettes.size();
    }

    result.atlases.reserve(input.assets.size());
    for (const SpriteAssetCatalogEntry& entry : input.assets)
        if (entry.asset != nullptr) result.atlases.push_back(analyze_atlas(*entry.asset));

    std::map<std::string, std::uint64_t, std::less<>> textureReferences;
    std::map<std::string, std::uint64_t, std::less<>> paletteReferences;
    for (const SpriteAssetCatalogEntry& entry : input.assets) {
        if (entry.asset == nullptr) continue;
        if (!entry.asset->textureAsset.empty()) ++textureReferences[entry.asset->textureAsset];
        if (!entry.asset->paletteAsset.empty()) ++paletteReferences[entry.asset->paletteAsset];
    }
    if (input.sprites != nullptr) {
        for (const SpriteDrawItem& item : input.sprites->items) {
            if (!item.textureAsset.empty()) ++textureReferences[item.textureAsset];
            if (!item.paletteAsset.empty()) ++paletteReferences[item.paletteAsset];
        }
    }
    for (const auto& [asset, references] : textureReferences) {
        const bool resident = contains(input.residentTextures, asset);
        const auto bytes = input.textureBytes.find(asset);
        const std::uint64_t estimated = bytes == input.textureBytes.end() ? 0U : bytes->second;
        result.residency.textures.push_back({asset, resident, estimated, references});
        if (resident) result.residency.residentTextureBytes += estimated;
        else ++result.residency.missingTextures;
    }
    for (const auto& [asset, references] : paletteReferences) {
        const bool resident = contains(input.residentPalettes, asset);
        const auto bytes = input.paletteBytes.find(asset);
        const std::uint64_t estimated = bytes == input.paletteBytes.end() ? 0U : bytes->second;
        result.residency.palettes.push_back({asset, resident, estimated, references});
        if (resident) result.residency.residentPaletteBytes += estimated;
        else ++result.residency.missingPalettes;
    }

    audit_references(input, result.referenceIssues);
    result.overdraw = render_sprite_overdraw_reference(input);
    result.visiblePixelCount = result.overdraw.coveredPixels;
    result.fragmentCount = result.overdraw.fragmentCount;

    append_budget_violation(result.budgetViolations, "sprites", result.spriteCount,
                            result.budget.maximumSprites);
    append_budget_violation(result.budgetViolations, "tiles", result.tileCount,
                            result.budget.maximumTiles);
    append_budget_violation(result.budgetViolations, "draw calls", result.drawCallCount,
                            result.budget.maximumDrawCalls);
    append_budget_violation(result.budgetViolations, "batches", result.batchCount,
                            result.budget.maximumBatches);
    append_budget_violation(result.budgetViolations, "visible pixels", result.visiblePixelCount,
                            result.budget.maximumVisiblePixels);
    append_budget_violation(result.budgetViolations, "fragments", result.fragmentCount,
                            result.budget.maximumFragments);
    append_budget_violation(result.budgetViolations, "textures", result.textureCount,
                            result.budget.maximumTextures);
    append_budget_violation(result.budgetViolations, "palettes", result.paletteCount,
                            result.budget.maximumPalettes);
    append_budget_violation(result.budgetViolations, "maximum overdraw",
                            result.overdraw.maximumOverdraw,
                            result.budget.maximumOverdraw);

    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, result.cameraName);
    hash_integer(hash, static_cast<std::uint8_t>(result.profile));
    for (const std::uint64_t value : {result.spriteCount, result.tileCount, result.batchCount,
                                      result.drawCallCount, result.visiblePixelCount,
                                      result.fragmentCount, result.textureCount,
                                      result.paletteCount, result.overdraw.contentHash})
        hash_integer(hash, value);
    for (const SpriteBatchBreakDiagnostic& item : result.batchBreaks) {
        hash_integer(hash, item.itemIndex);
        hash_integer(hash, static_cast<std::uint16_t>(item.reasons));
    }
    for (const SpriteReferenceIssue& issue : result.referenceIssues) {
        hash_integer(hash, static_cast<std::uint8_t>(issue.kind));
        hash_string(hash, issue.reference);
        hash_string(hash, issue.location);
    }
    result.contentHash = hash;
    return result;
}

} // namespace dve
