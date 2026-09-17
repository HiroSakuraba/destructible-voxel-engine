#include "dve/brick_palette.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Exactly representable weights so that reordering a summation cannot change the result and the
// determinism assertions below stay meaningful rather than luck of float association.
BrickSourceSurface two_material_surface() {
    BrickSourceSurface surface;
    surface.brickId = 41U;
    surface.assetName = "ConcreteWall";
    surface.samples.push_back({{{7U, 0.75F}, {3U, 0.25F}}});
    surface.samples.push_back({{{7U, 0.5F}, {3U, 0.5F}}});
    surface.samples.push_back({{{7U, 1.0F}}});
    return surface;
}

std::uint32_t weight_sum(const BrickPaletteSampleEncoding& sample) {
    std::uint32_t total = 0U;
    for (std::uint8_t index = 0U; index < sample.usedSlots; ++index)
        total += sample.quantizedWeights[index];
    return total;
}

void test_basic_cook_and_ranking() {
    BrickPaletteCookConfig config;
    const auto palette = cook_brick_palette(two_material_surface(), config);

    require(palette.valid, "two material cook rejected");
    require(palette.encoding == BrickPaletteEncoding::Palette2, "encoding is not palette2");
    require(palette.runtimePath == VoxelMaterialRuntimePath::DeferredPalette2,
            "runtime path does not match encoding");
    require(palette.slots.size() == 2U, "slot count is not two");
    require(palette.slots[0] == 7U, "dominant material is not slot zero");
    require(palette.slots[1] == 3U, "second material is not slot one");
    require(palette.sourceMaterialCount == 2U, "source material count wrong");
    require(palette.canonicalMaterials.size() == 2U, "canonical membership not preserved");
    require(palette.canonicalMaterials[0] == 3U && palette.canonicalMaterials[1] == 7U,
            "canonical membership is not ascending");
    require(palette.canonicalSamples.size() == 3U,
            "full canonical source samples were not preserved");
    require(palette.canonicalSamples[0].contributions.size() == 2U,
            "canonical source contribution count is wrong");
    require(!palette.overflowed, "two materials reported overflow");
    require(palette.samples.size() == 3U, "sample count wrong");

    for (const auto& sample : palette.samples)
        require(weight_sum(sample) == kBrickPaletteWeightDenominator,
                "quantized weights do not sum to the denominator");

    require(palette.samples[2].usedSlots == 1U, "single contribution sample kept extra slots");
    require(palette.samples[2].slotIndices[0] == 0U, "single contribution sample used wrong slot");
    require(palette.maximumWeightError <= 1.0F / 255.0F + 1.0e-6F,
            "quantization error exceeded one denominator step");
    require(palette.maximumReductionError == 0.0F, "reduction error reported without reduction");
}

void test_contribution_order_independence() {
    BrickPaletteCookConfig config;
    BrickSourceSurface forward = two_material_surface();
    BrickSourceSurface reversed = forward;
    for (auto& sample : reversed.samples)
        std::reverse(sample.contributions.begin(), sample.contributions.end());

    const auto a = cook_brick_palette(forward, config);
    const auto b = cook_brick_palette(reversed, config);
    require(a.contentHash == b.contentHash, "cook is sensitive to contribution order");
    require(serialize_brick_palette(a) == serialize_brick_palette(b),
            "serialized payload is sensitive to contribution order");
}

void test_adversarial_duplicate_order_independence() {
    BrickSourceSurface forward;
    forward.brickId = 99U;
    forward.assetName = "AdversarialOrder";
    forward.samples.push_back({{{1U, 100000000.0F}, {1U, 4.0F}, {1U, 4.0F},
                                {2U, 100000000.0F}, {2U, 8.0F}}});
    BrickSourceSurface reversed = forward;
    std::reverse(reversed.samples[0].contributions.begin(),
                 reversed.samples[0].contributions.end());

    BrickPaletteCookConfig config;
    config.maximumPaletteSlots = 2U;
    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    const auto a = cook_brick_palette(forward, config);
    const auto b = cook_brick_palette(reversed, config);
    require(a.slots == b.slots, "adversarial duplicate ordering changed palette ranking");
    require(a.contentHash == b.contentHash,
            "adversarial duplicate ordering changed the content hash");
    require(serialize_brick_palette(a) == serialize_brick_palette(b),
            "adversarial duplicate ordering changed serialized bytes");
}

void test_epsilon_and_single_material() {
    BrickSourceSurface surface;
    surface.brickId = 5U;
    surface.assetName = "MetalCrate";
    surface.samples.push_back({{{2U, 1.0F}, {9U, 1.0e-6F}}});

    BrickPaletteCookConfig config;
    config.weightEpsilon = 1.0e-4F;
    const auto palette = cook_brick_palette(surface, config);
    require(palette.encoding == BrickPaletteEncoding::Single, "epsilon did not drop noise material");
    require(palette.runtimePath == VoxelMaterialRuntimePath::SingleMaterial,
            "single material path not selected");
    require(palette.sourceMaterialCount == 1U, "noise material counted as source material");
    require(palette.samples[0].usedSlots == 1U, "single material sample used extra slots");
}

void test_empty_surface() {
    BrickSourceSurface surface;
    surface.brickId = 6U;
    surface.assetName = "Void";
    surface.samples.push_back({{}});

    const auto palette = cook_brick_palette(surface, BrickPaletteCookConfig{});
    require(palette.valid, "empty surface rejected");
    require(palette.encoding == BrickPaletteEncoding::Empty, "empty surface produced a palette");
    require(palette.runtimePath == VoxelMaterialRuntimePath::NoPath, "empty surface claimed a path");
    require(palette.samples.empty(), "empty surface emitted samples");
}

BrickSourceSurface six_material_surface() {
    BrickSourceSurface surface;
    surface.brickId = 77U;
    surface.assetName = "HeroRock";
    surface.samples.push_back({{{1U, 0.5F}, {2U, 0.25F}, {3U, 0.125F}, {4U, 0.0625F},
                               {5U, 0.03125F}, {6U, 0.03125F}}});
    surface.samples.push_back({{{1U, 0.75F}, {5U, 0.25F}}});
    return surface;
}

void test_overflow_policies() {
    BrickPaletteCookConfig config;

    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    auto dominant = cook_brick_palette(six_material_surface(), config);
    require(dominant.valid && dominant.overflowed, "dominant policy did not report overflow");
    require(dominant.encoding == BrickPaletteEncoding::Palette4, "dominant policy lost palette4");
    require(dominant.slots.size() == 4U, "dominant policy kept the wrong slot count");
    // Accumulated weights are 1:1.25, 5:0.28125, 2:0.25, 3:0.125, 4:0.0625, 6:0.03125.
    require(dominant.slots[0] == 1U && dominant.slots[1] == 5U && dominant.slots[2] == 2U &&
                dominant.slots[3] == 3U,
            "dominant ranking is wrong");
    require(dominant.sourceMaterialCount == 6U, "canonical source count was truncated");
    require(dominant.canonicalMaterials.size() == 6U,
            "canonical membership did not survive reduction");
    require(dominant.overflowSampleCount == 1U, "overflow sample count wrong");
    require(dominant.maximumReductionError > 0.0F, "reduction error not measured");
    for (const auto& sample : dominant.samples)
        require(weight_sum(sample) == kBrickPaletteWeightDenominator,
                "reduced sample weights were not renormalized");

    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::BakeProperties;
    const auto baked = cook_brick_palette(six_material_surface(), config);
    require(baked.valid && baked.runtimePath == VoxelMaterialRuntimePath::BakedProperties,
            "bake policy did not fall back to baked properties");
    require(baked.samples.empty() && baked.slots.empty(), "bake fallback emitted palette data");

    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::RequestRecook;
    const auto recook = cook_brick_palette(six_material_surface(), config);
    require(recook.valid && recook.recookRequested, "recook policy did not request a recook");
    require(recook.runtimePath == VoxelMaterialRuntimePath::NoPath,
            "recook policy claimed a runtime path");

    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::Reject;
    const auto rejected = cook_brick_palette(six_material_surface(), config);
    require(!rejected.valid, "reject policy accepted an overflowing brick");
    require(!rejected.reason.empty(), "reject policy gave no reason");
}

void test_slot_limit() {
    BrickPaletteCookConfig config;
    config.maximumPaletteSlots = 2U;
    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    const auto palette = cook_brick_palette(six_material_surface(), config);
    require(palette.encoding == BrickPaletteEncoding::Palette2,
            "two slot limit did not force palette2");
    require(palette.slots.size() == 2U, "two slot limit kept extra slots");
    require(palette.slots[0] == 1U && palette.slots[1] == 5U, "two slot limit kept the wrong pair");
    require(palette.samples[0].usedSlots == 2U, "first sample lost a surviving slot");
    require(palette.samples[1].usedSlots == 2U, "second sample lost a surviving slot");
    require(palette.overflowSampleCount == 1U,
            "only the sample that lost material mass should count as an overflow sample");
    for (const auto& sample : palette.samples)
        require(weight_sum(sample) == kBrickPaletteWeightDenominator,
                "two slot limit left unnormalized weights");
}

void test_degenerate_sample_fallback() {
    BrickSourceSurface surface;
    surface.brickId = 12U;
    surface.assetName = "Speckle";
    // Slot ranking is driven by the first sample. The second sample references only materials that
    // do not survive reduction, so it must resolve to the dominant slot rather than nothing.
    surface.samples.push_back({{{1U, 1.0F}, {2U, 0.5F}, {3U, 0.25F}, {4U, 0.125F}}});
    surface.samples.push_back({{{9U, 0.05F}}});

    BrickPaletteCookConfig config;
    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    const auto palette = cook_brick_palette(surface, config);
    require(palette.degenerateSampleCount == 1U, "degenerate sample not counted");
    require(palette.samples[1].usedSlots == 1U, "degenerate sample did not collapse to one slot");
    require(palette.samples[1].slotIndices[0] == 0U,
            "degenerate sample did not use the dominant slot");
    require(palette.samples[1].quantizedWeights[0] == kBrickPaletteWeightDenominator,
            "degenerate sample weight is not full");
}

void test_remap_table() {
    BrickPaletteCookConfig config;
    const auto first = cook_brick_palette(two_material_surface(), config);

    BrickSourceSurface second;
    second.brickId = 42U;
    second.assetName = "GoldVein";
    second.samples.push_back({{{11U, 0.5F}, {3U, 0.5F}}});
    const auto secondPalette = cook_brick_palette(second, config);

    const std::vector<CookedBrickPalette> bricks{first, secondPalette};
    const auto table = build_brick_palette_remap(bricks);
    require(table.globalMaterialIds.size() == 3U, "remap union size wrong");
    require(table.globalMaterialIds[0] == 3U && table.globalMaterialIds[1] == 7U &&
                table.globalMaterialIds[2] == 11U,
            "remap union is not ascending and deduplicated");

    const auto firstMap = remap_brick_palette_slots(first, table);
    require(firstMap.size() == 2U, "first brick remap size wrong");
    require(firstMap[0] == 1U && firstMap[1] == 0U, "first brick remap indices wrong");

    const auto secondMap = remap_brick_palette_slots(secondPalette, table);
    require(secondMap.size() == 2U, "second brick remap size wrong");
    require(secondMap[0] == 0U && secondMap[1] == 2U, "second brick remap indices wrong");

    BrickPaletteRemapTable partial;
    partial.globalMaterialIds = {3U};
    require(remap_brick_palette_slots(first, partial).empty(),
            "remap did not reject an incomplete table");
}

void test_serialization_round_trip() {
    const auto palette = cook_brick_palette(two_material_surface(), BrickPaletteCookConfig{});
    const auto bytes = serialize_brick_palette(palette);

    CookedBrickPalette decoded;
    std::string error;
    require(deserialize_brick_palette(bytes, decoded, &error), "round trip failed: " + error);
    require(decoded.sourceFormatVersion == kBrickPaletteFormatVersion, "source version wrong");
    require(decoded.brickId == palette.brickId, "brick id lost");
    require(decoded.assetName == palette.assetName, "asset name lost");
    require(decoded.slots == palette.slots, "slots lost");
    require(decoded.canonicalMaterials == palette.canonicalMaterials, "canonical membership lost");
    require(decoded.canonicalSamples.size() == palette.canonicalSamples.size(),
            "canonical source sample count lost");
    require(decoded.canonicalSamples[0].contributions.size() ==
                palette.canonicalSamples[0].contributions.size(),
            "canonical source contributions lost");
    require(decoded.canonicalSamples[0].contributions[0].globalMaterialId ==
                palette.canonicalSamples[0].contributions[0].globalMaterialId &&
            decoded.canonicalSamples[0].contributions[0].weight ==
                palette.canonicalSamples[0].contributions[0].weight,
            "canonical source contribution changed across round trip");
    require(decoded.samples.size() == palette.samples.size(), "sample count lost");
    require(decoded.contentHash == palette.contentHash, "content hash changed across round trip");
    require(serialize_brick_palette(decoded) == bytes, "re-serialization is not byte stable");
}

void test_serialization_rejects_damage() {
    const auto palette = cook_brick_palette(two_material_surface(), BrickPaletteCookConfig{});
    const auto bytes = serialize_brick_palette(palette);
    CookedBrickPalette decoded;

    auto corrupted = bytes;
    require(corrupted.size() > 12U, "payload unexpectedly small");
    corrupted[corrupted.size() - 10U] = static_cast<std::uint8_t>(corrupted[corrupted.size() - 10U] ^ 0xFFU);
    require(!deserialize_brick_palette(corrupted, decoded), "damaged payload accepted");

    auto truncated = bytes;
    truncated.resize(truncated.size() / 2U);
    require(!deserialize_brick_palette(truncated, decoded), "truncated payload accepted");

    auto badMagic = bytes;
    badMagic[0] = static_cast<std::uint8_t>('X');
    require(!deserialize_brick_palette(badMagic, decoded), "bad magic accepted");

    auto badVersion = bytes;
    badVersion[kBrickPaletteMagic.size() + 1U] =
        static_cast<std::uint8_t>(kBrickPaletteFormatVersion + 1U);
    require(!deserialize_brick_palette(badVersion, decoded), "future version accepted");

    std::vector<std::uint8_t> empty;
    require(!deserialize_brick_palette(empty, decoded), "empty payload accepted");
}

void test_version_zero_migration() {
    const auto palette = cook_brick_palette(two_material_surface(), BrickPaletteCookConfig{});
    require(palette.samples[0].quantizedWeights[0] != palette.samples[0].quantizedWeights[1],
            "test fixture does not have asymmetric weights");

    const auto legacy = serialize_brick_palette(palette, 0U);
    require(legacy.size() < serialize_brick_palette(palette).size(),
            "legacy payload is not smaller than the current payload");

    CookedBrickPalette migrated;
    std::string error;
    require(deserialize_brick_palette(legacy, migrated, &error), "migration failed: " + error);
    require(migrated.sourceFormatVersion == 0U, "migration did not report the source version");
    require(migrated.formatVersion == kBrickPaletteFormatVersion,
            "migrated record does not carry the current format version");
    require(migrated.slots == palette.slots, "migration lost the palette table");
    require(migrated.samples.size() == palette.samples.size(), "migration lost samples");
    require(migrated.samples[0].usedSlots == 2U, "migration lost slot usage");
    require(migrated.samples[0].quantizedWeights[0] == 128U &&
                migrated.samples[0].quantizedWeights[1] == 127U,
            "migration did not reconstruct a uniform split");
    require(migrated.reason.find("version 0") != std::string::npos,
            "migration did not record the provenance in the reason");
}

void test_version_one_migration() {
    const auto palette = cook_brick_palette(two_material_surface(), BrickPaletteCookConfig{});
    const auto legacy = serialize_brick_palette(palette, 1U);
    CookedBrickPalette migrated;
    std::string error;
    require(deserialize_brick_palette(legacy, migrated, &error),
            "version one migration failed: " + error);
    require(migrated.sourceFormatVersion == 1U, "version one provenance was lost");
    require(migrated.formatVersion == kBrickPaletteFormatVersion,
            "version one record did not migrate to the current format");
    require(migrated.canonicalSamples.empty(),
            "version one migration invented unavailable per-sample source weights");
    require(migrated.slots == palette.slots && migrated.samples.size() == palette.samples.size(),
            "version one migration lost renderable palette data");
}

void test_report_and_json() {
    std::vector<BrickSourceSurface> surfaces;
    surfaces.push_back(two_material_surface());
    surfaces.push_back(six_material_surface());

    BrickSourceSurface single;
    single.brickId = 3U;
    single.assetName = "MetalCrate";
    single.samples.push_back({{{2U, 1.0F}}});
    surfaces.push_back(single);

    BrickPaletteCookConfig config;
    config.overflowPolicy = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
    const auto report = build_brick_palette_cook_report(surfaces, config);

    require(report.brickCount == 3U, "brick count wrong");
    require(report.palette2BrickCount == 1U, "palette2 count wrong");
    require(report.palette4BrickCount == 1U, "palette4 count wrong");
    require(report.singleMaterialBrickCount == 1U, "single material count wrong");
    require(report.overflowBrickCount == 1U, "overflow brick count wrong");
    require(report.rejectedBrickCount == 0U, "unexpected rejection");
    require(report.sampleCount == 6U, "sample count wrong");
    require(report.paletteSlotCount == 7U, "palette slot count wrong");
    require(report.payloadBytes > 0U, "payload byte total not measured");

    const auto repeat = build_brick_palette_cook_report(surfaces, config);
    require(repeat.contentHash == report.contentHash, "report hash is not deterministic");

    const std::string json = brick_palette_cook_json(report);
    require(json.find("\"palette4BrickCount\": 1") != std::string::npos,
            "json missing palette4 count");
    require(json.find("\"encoding\": \"palette2\"") != std::string::npos,
            "json missing palette2 encoding");
    require(json.find("\"contentHash\"") != std::string::npos, "json missing content hash");
}

} // namespace

int main() {
    try {
        test_basic_cook_and_ranking();
        test_contribution_order_independence();
        test_adversarial_duplicate_order_independence();
        test_epsilon_and_single_material();
        test_empty_surface();
        test_overflow_policies();
        test_slot_limit();
        test_degenerate_sample_fallback();
        test_remap_table();
        test_serialization_round_trip();
        test_serialization_rejects_damage();
        test_version_zero_migration();
        test_version_one_migration();
        test_report_and_json();
    } catch (const std::exception& error) {
        std::cerr << "brick palette v2.25 test failure: " << error.what() << "\n";
        return 1;
    }
    std::cout << "brick palette v2.25 tests passed\n";
    return 0;
}
