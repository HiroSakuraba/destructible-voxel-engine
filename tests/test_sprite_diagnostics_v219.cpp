#include "dve/sprite_diagnostics.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace dve;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

SpriteDrawItem make_item(SpriteOwnerId owner, std::string texture, float left, float bottom,
                         float right, float top) {
    SpriteDrawItem item;
    item.owner = owner;
    item.asset = 1U;
    item.frame = 0U;
    item.textureAsset = std::move(texture);
    item.materialId = 7U;
    item.paletteAsset = "palettes/test.dvepalette";
    item.paletteBank = 0U;
    item.paletteStateHash = 10U;
    item.sampling = SpriteSampling::Nearest;
    item.blendMode = SpriteBlendMode::Alpha;
    item.sortingLayer = 2;
    item.orderInLayer = 0;
    item.sortDepth = 0.0F;
    item.vertices[0].position = {left, bottom, 0.0F};
    item.vertices[1].position = {right, bottom, 0.0F};
    item.vertices[2].position = {right, top, 0.0F};
    item.vertices[3].position = {left, top, 0.0F};
    return item;
}

void test_complete_diagnostics() {
    SpriteAsset asset;
    asset.name = "diagnostic_cast";
    asset.textureAsset = "textures/cast.png";
    asset.textureWidth = 40U;
    asset.textureHeight = 16U;
    asset.pixelsPerWorldUnit = 16.0F;
    asset.paletteAsset = "palettes/test.dvepalette";
    asset.frames = {
        SpriteFrame{"idle", {0U, 0U, 12U, 12U}, 12U, 12U, 0, 0, {6.0F, 0.0F}, 0.1F, {}},
        SpriteFrame{"run", {10U, 0U, 12U, 12U}, 12U, 12U, 0, 0, {6.0F, 0.0F}, 0.1F, {}},
        SpriteFrame{"jump", {24U, 0U, 12U, 12U}, 12U, 12U, 0, 0, {6.0F, 0.0F}, 0.1F, {}},
    };
    SpriteClip idle;
    idle.name = "idle";
    idle.frames = {0U};
    SpriteSocketKey hand;
    hand.id = 1U;
    hand.name = "hand";
    hand.sequenceIndex = 0U;
    idle.socketKeys.push_back(hand);
    asset.clips.push_back(idle);

    SpriteRenderList render;
    render.items.push_back(make_item(1U, "textures/cast.png", -0.5F, -0.5F, 0.5F, 0.5F));
    render.items.push_back(make_item(2U, "textures/cast.png", -0.25F, -0.25F, 0.75F, 0.75F));
    render.items.push_back(make_item(3U, "textures/missing.png", 0.0125F, 0.0F, 0.5125F, 0.5F));
    render.items[1].paletteBank = 1U;
    render.items[2].materialId = 8U;
    render.items[2].blendMode = SpriteBlendMode::Additive;
    render.items[2].sortingLayer = 3;
    render.batches = {
        SpriteBatch{"textures/cast.png", 7U, 0U, SpriteSampling::Nearest,
                    SpriteBlendMode::Alpha, 0U, 1U, "palettes/test.dvepalette", 10U, 0U},
        SpriteBatch{"textures/cast.png", 7U, 1U, SpriteSampling::Nearest,
                    SpriteBlendMode::Alpha, 1U, 1U, "palettes/test.dvepalette", 10U, 0U},
        SpriteBatch{"textures/missing.png", 8U, 0U, SpriteSampling::Nearest,
                    SpriteBlendMode::Additive, 2U, 1U, "palettes/test.dvepalette", 10U, 0U},
    };

    TileRenderItem tile;
    tile.layerIndex = 0U;
    tile.col = 1U;
    tile.row = 1U;
    tile.tileValue = 1U;
    tile.atlasIndex = 0U;
    tile.worldBounds = {{-8.0F, -8.0F}, {16.0F, 16.0F}};
    std::vector<TileRenderItem> tiles{tile};

    SpriteAnimationStateMachineAsset machine;
    machine.name = "broken_machine";
    machine.initialState = "missing_initial";
    machine.states = {SpriteAnimationState{"Idle", "idle", 1.0F, {}},
                      SpriteAnimationState{"Broken", "missing_clip", 1.0F, {}}};
    SpriteAnimationTransition transition;
    transition.fromState = "missing_from";
    transition.toState = "missing_to";
    SpriteAnimationCondition missingCondition;
    missingCondition.parameter = "missing_parameter";
    missingCondition.operation = SpriteAnimationCompareOp::IsTrue;
    missingCondition.comparison.type = SpriteAnimationParameterType::Boolean;
    missingCondition.comparison.booleanValue = true;
    transition.conditions.push_back(missingCondition);
    machine.transitions.push_back(transition);

    std::vector<SpriteAssetCatalogEntry> assets{{1U, &asset}};
    std::vector<SpriteMachineReference> machines{{"player", 1U, &machine}};
    std::vector<SpriteInstanceReference> instances{{10U, 1U, "missing_instance_clip", "Idle"}};
    std::vector<SpriteSocketReference> sockets{{10U, 1U, "idle", "missing_socket"}};
    std::vector<std::string> residentTextures{"textures/cast.png"};
    std::vector<std::string> residentPalettes;

    SpriteDiagnosticsInput input;
    input.cameraName = "Gameplay";
    input.sprites = &render;
    input.tiles = tiles;
    input.assets = assets;
    input.machines = machines;
    input.instances = instances;
    input.socketReferences = sockets;
    input.residentTextures = residentTextures;
    input.residentPalettes = residentPalettes;
    input.textureBytes["textures/cast.png"] = 2048U;
    input.textureBytes["textures/missing.png"] = 1024U;
    input.paletteBytes["palettes/test.dvepalette"] = 256U;
    input.presentation.logicalWidth = 64U;
    input.presentation.logicalHeight = 64U;
    input.presentation.pixelsPerWorldUnit = 16.0F;
    input.logicalWidth = 64U;
    input.logicalHeight = 64U;
    input.profile = SpriteBudgetProfile::Retro8Bit;
    input.budget = SpriteCameraBudget{2U, 0U, 2U, 2U, 32U, 64U, 1U, 0U, 2U};

    const SpriteDiagnosticsReport report = build_sprite_diagnostics(input);
    require(report.spriteCount == 3U && report.tileCount == 1U,
            "counts sprites and tiles");
    require(report.drawCallCount == 3U && report.batchCount == 3U,
            "reports draw calls and batches");
    require(report.batchBreaks.size() == 2U,
            "reports every adjacent batch break");
    require(has_batch_break_reason(report.batchBreaks[0].reasons,
                                   SpriteBatchBreakReason::PaletteBank),
            "identifies palette bank batch break");
    require(has_batch_break_reason(report.batchBreaks[1].reasons,
                                   SpriteBatchBreakReason::Texture) &&
            has_batch_break_reason(report.batchBreaks[1].reasons,
                                   SpriteBatchBreakReason::BlendMode),
            "identifies compound batch break");
    require(report.atlases.size() == 1U && report.atlases[0].exact,
            "computes exact atlas occupancy");
    require(report.atlases[0].occupiedPixels == 408U &&
            report.atlases[0].overlappingPixels == 24U,
            "computes atlas overlap and occupied pixels");
    require(report.atlases[0].packingFragmentation > 0.0,
            "computes atlas fragmentation");
    require(report.residency.missingTextures == 1U &&
            report.residency.missingPalettes == 1U,
            "reports missing texture and palette residency");
    require(report.sortingLayers.size() == 2U,
            "builds sorting layer visualization groups");
    require(!report.pixelSnapViolations.empty(),
            "detects off-grid sprite vertices");
    require(report.visiblePixelCount > 0U && report.fragmentCount > report.visiblePixelCount &&
            report.overdraw.maximumOverdraw >= 2U,
            "reference overdraw counts visible pixels and fragments");
    require(report.overdraw.heatmapRgba8.size() == 64U * 64U * 4U &&
            report.overdraw.contentHash != 0U,
            "creates deterministic RGBA overdraw heatmap");
    require(report.referenceIssues.size() >= 7U,
            "reports missing texture, palette, clip, socket, state, and parameter references");
    const auto has_kind = [&](SpriteReferenceKind kind) {
        return std::any_of(report.referenceIssues.begin(), report.referenceIssues.end(),
                           [&](const SpriteReferenceIssue& issue) { return issue.kind == kind; });
    };
    require(has_kind(SpriteReferenceKind::Texture) && has_kind(SpriteReferenceKind::Palette) &&
            has_kind(SpriteReferenceKind::Clip) && has_kind(SpriteReferenceKind::Socket) &&
            has_kind(SpriteReferenceKind::State) && has_kind(SpriteReferenceKind::Parameter),
            "covers all requested missing-reference categories");
    require(report.budgetViolations.size() >= 5U,
            "reports camera/profile budget violations");

    const SpriteDiagnosticsReport repeated = build_sprite_diagnostics(input);
    require(report.contentHash == repeated.contentHash &&
            report.overdraw.contentHash == repeated.overdraw.contentHash,
            "diagnostic and heatmap hashes are deterministic");
}

} // namespace

int main() {
    test_complete_diagnostics();
    if (failures != 0) {
        std::cerr << failures << " sprite diagnostics test(s) failed\n";
        return 1;
    }
    std::cout << "sprite diagnostics v2.19 tests passed\n";
    return 0;
}
