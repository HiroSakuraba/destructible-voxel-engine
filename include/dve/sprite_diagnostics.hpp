#pragma once

#include "dve/sprite_animation.hpp"
#include "dve/tilemap2d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve {

enum class SpriteDiagnosticSeverity : std::uint8_t { Information, Warning, Error };

enum class SpriteBudgetProfile : std::uint8_t {
    Unrestricted,
    Retro8Bit,
    Retro16Bit,
    Handheld,
    Mobile,
    Desktop,
};

struct SpriteCameraBudget {
    std::uint64_t maximumSprites{};
    std::uint64_t maximumTiles{};
    std::uint64_t maximumDrawCalls{};
    std::uint64_t maximumBatches{};
    std::uint64_t maximumVisiblePixels{};
    std::uint64_t maximumFragments{};
    std::uint64_t maximumTextures{};
    std::uint64_t maximumPalettes{};
    std::uint16_t maximumOverdraw{};
};

[[nodiscard]] SpriteCameraBudget sprite_camera_budget_for_profile(
    SpriteBudgetProfile profile, std::uint32_t logicalWidth, std::uint32_t logicalHeight) noexcept;

enum class SpriteBatchBreakReason : std::uint16_t {
    NoBreak = 0U,
    Texture = 1U << 0U,
    Material = 1U << 1U,
    PaletteBank = 1U << 2U,
    PaletteAsset = 1U << 3U,
    PaletteState = 1U << 4U,
    Sampling = 1U << 5U,
    BlendMode = 1U << 6U,
};

[[nodiscard]] constexpr SpriteBatchBreakReason operator|(
    SpriteBatchBreakReason left, SpriteBatchBreakReason right) noexcept {
    return static_cast<SpriteBatchBreakReason>(
        static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}
[[nodiscard]] constexpr bool has_batch_break_reason(
    SpriteBatchBreakReason value, SpriteBatchBreakReason reason) noexcept {
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(reason)) != 0U;
}

struct SpriteBatchBreakDiagnostic {
    std::size_t itemIndex{};
    SpriteOwnerId previousOwner{kInvalidSpriteOwnerId};
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteBatchBreakReason reasons{SpriteBatchBreakReason::NoBreak};
    std::string summary;
};

struct SpriteAtlasDiagnostic {
    std::string assetName;
    std::string textureAsset;
    std::uint32_t textureWidth{};
    std::uint32_t textureHeight{};
    std::uint64_t atlasPixels{};
    std::uint64_t occupiedPixels{};
    std::uint64_t overlappingPixels{};
    std::uint64_t boundingPixels{};
    std::uint64_t freePixels{};
    std::size_t frameCount{};
    double occupancy{};
    double packingFragmentation{};
    bool exact{};
};

struct SpriteResidencyEntry {
    std::string asset;
    bool resident{};
    std::uint64_t estimatedBytes{};
    std::uint64_t references{};
};

struct SpriteResidencyDiagnostic {
    std::vector<SpriteResidencyEntry> textures;
    std::vector<SpriteResidencyEntry> palettes;
    std::uint64_t residentTextureBytes{};
    std::uint64_t residentPaletteBytes{};
    std::uint64_t missingTextures{};
    std::uint64_t missingPalettes{};
};

struct SpriteSortingLayerDiagnostic {
    std::int32_t sortingLayer{};
    std::int32_t minimumOrder{};
    std::int32_t maximumOrder{};
    float minimumDepth{};
    float maximumDepth{};
    std::vector<SpriteOwnerId> owners;
    std::array<std::uint8_t, 4> displayColor{};
    bool ambiguousOrdering{};
};

struct SpritePixelSnapViolation {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    std::size_t vertexIndex{};
    char axis{'x'};
    float projectedPixel{};
    float distanceToPixel{};
};

enum class SpriteReferenceKind : std::uint8_t {
    Texture,
    Palette,
    Clip,
    Socket,
    State,
    Parameter,
};

struct SpriteReferenceIssue {
    SpriteDiagnosticSeverity severity{SpriteDiagnosticSeverity::Error};
    SpriteReferenceKind kind{SpriteReferenceKind::Texture};
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAssetId asset{kInvalidSpriteAssetId};
    std::string reference;
    std::string location;
    std::string message;
};

struct SpriteAssetCatalogEntry {
    SpriteAssetId id{kInvalidSpriteAssetId};
    const SpriteAsset* asset{};
};

struct SpriteInstanceReference {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAssetId asset{kInvalidSpriteAssetId};
    std::string clip;
    std::string state;
};

struct SpriteSocketReference {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAssetId asset{kInvalidSpriteAssetId};
    std::string clip;
    std::string socket;
};

struct SpriteMachineReference {
    std::string name;
    SpriteAssetId spriteAsset{kInvalidSpriteAssetId};
    const SpriteAnimationStateMachineAsset* machine{};
};

struct SpriteOverdrawImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint16_t> samples;
    std::vector<std::byte> heatmapRgba8;
    std::uint64_t coveredPixels{};
    std::uint64_t fragmentCount{};
    std::uint16_t maximumOverdraw{};
    double averageOverdrawOnCoveredPixels{};
    std::uint64_t contentHash{};
};

struct SpriteBudgetViolation {
    SpriteDiagnosticSeverity severity{SpriteDiagnosticSeverity::Warning};
    std::string metric;
    std::uint64_t measured{};
    std::uint64_t limit{};
    std::string message;
};

struct SpriteDiagnosticsInput {
    std::string cameraName{"Main Camera"};
    const SpriteRenderList* sprites{};
    std::span<const TileRenderItem> tiles;
    std::span<const SpriteAssetCatalogEntry> assets;
    std::span<const SpriteMachineReference> machines;
    std::span<const SpriteInstanceReference> instances;
    std::span<const SpriteSocketReference> socketReferences;
    std::span<const std::string> residentTextures;
    std::span<const std::string> residentPalettes;
    std::map<std::string, std::uint64_t, std::less<>> textureBytes;
    std::map<std::string, std::uint64_t, std::less<>> paletteBytes;
    PixelPresentationConfig presentation{};
    GameplayPlane2D plane{GameplayPlane2D::XY};
    Float3 cameraOrigin{};
    TileVec2 tileCameraCenterPixels{};
    std::uint32_t logicalWidth{320U};
    std::uint32_t logicalHeight{180U};
    SpriteBudgetProfile profile{SpriteBudgetProfile::Desktop};
    std::optional<SpriteCameraBudget> budget;
    bool includeTilesInOverdraw{true};
};

struct SpriteDiagnosticsReport {
    std::string cameraName;
    SpriteBudgetProfile profile{SpriteBudgetProfile::Desktop};
    SpriteCameraBudget budget{};
    std::uint64_t spriteCount{};
    std::uint64_t tileCount{};
    std::uint64_t batchCount{};
    std::uint64_t drawCallCount{};
    std::uint64_t visiblePixelCount{};
    std::uint64_t fragmentCount{};
    std::uint64_t textureCount{};
    std::uint64_t paletteCount{};
    std::vector<SpriteBatchBreakDiagnostic> batchBreaks;
    std::vector<SpriteAtlasDiagnostic> atlases;
    SpriteResidencyDiagnostic residency;
    std::vector<SpriteSortingLayerDiagnostic> sortingLayers;
    std::vector<SpritePixelSnapViolation> pixelSnapViolations;
    std::vector<SpriteReferenceIssue> referenceIssues;
    std::vector<SpriteBudgetViolation> budgetViolations;
    SpriteOverdrawImage overdraw;
    std::uint64_t contentHash{};
};

[[nodiscard]] SpriteOverdrawImage render_sprite_overdraw_reference(
    const SpriteDiagnosticsInput& input);
[[nodiscard]] SpriteDiagnosticsReport build_sprite_diagnostics(
    const SpriteDiagnosticsInput& input);

} // namespace dve
