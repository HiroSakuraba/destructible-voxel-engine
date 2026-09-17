#include "dve/render/brick_palette_shading.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(float actual, float expected, float tolerance, const std::string& message) {
    if (!(std::fabs(actual - expected) <= tolerance))
        throw std::runtime_error(message + " (actual " + std::to_string(actual) + ", expected " +
                                 std::to_string(expected) + ")");
}

float vector_length(BrickPaletteFloat3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

std::vector<BrickPaletteMaterialRecord> flat_records() {
    BrickPaletteMaterialRecord red;
    red.globalMaterialId = 7U;
    red.baseColor = {0.8F, 0.1F, 0.1F};
    red.roughness = 0.2F;
    red.metallic = 0.0F;
    red.emissive = {0.0F, 0.0F, 0.0F};
    red.opacity = 1.0F;
    red.normal = {0.0F, 0.0F, 1.0F};

    BrickPaletteMaterialRecord blue;
    blue.globalMaterialId = 3U;
    blue.baseColor = {0.0F, 0.1F, 0.9F};
    blue.roughness = 0.8F;
    blue.metallic = 1.0F;
    blue.emissive = {0.2F, 0.0F, 0.0F};
    blue.opacity = 0.5F;
    blue.normal = {0.0F, 0.0F, 1.0F};

    return {red, blue};
}

CookedBrickPalette two_slot_palette(float dominantWeight) {
    BrickSourceSurface surface;
    surface.brickId = 1U;
    surface.assetName = "BlendTest";
    surface.samples.push_back({{{7U, dominantWeight}, {3U, 1.0F - dominantWeight}}});
    return cook_brick_palette(surface, BrickPaletteCookConfig{});
}

BrickPaletteSurfacePoint upward_point() {
    BrickPaletteSurfacePoint point;
    point.worldPosition = {0.25F, 1.5F, -0.75F};
    point.worldNormal = {0.0F, 1.0F, 0.0F};
    point.assetU = 0.3F;
    point.assetV = 0.7F;
    return point;
}

void test_texture_sample_counters() {
    require(brick_palette_texture_sample_count(1U, BrickPaletteMappingMode::AssetUv) == 5U,
            "single material asset uv sample count wrong");
    require(brick_palette_texture_sample_count(2U, BrickPaletteMappingMode::AssetUv) == 10U,
            "palette2 asset uv sample count wrong");
    require(brick_palette_texture_sample_count(4U, BrickPaletteMappingMode::AssetUv) == 20U,
            "palette4 asset uv sample count wrong");

    // The v2.24 policy estimate assumed one projection per channel. World triplanar costs three.
    require(brick_palette_texture_sample_count(2U, BrickPaletteMappingMode::WorldTriplanar) == 30U,
            "palette2 triplanar sample count wrong");
    require(brick_palette_texture_sample_count(4U, BrickPaletteMappingMode::WorldTriplanar) == 60U,
            "palette4 triplanar sample count wrong");
    require(brick_palette_texture_sample_count(0U, BrickPaletteMappingMode::WorldTriplanar) == 0U,
            "zero slots should cost nothing");
}

void test_single_slot_matches_source_material() {
    BrickSourceSurface surface;
    surface.brickId = 2U;
    surface.assetName = "SingleTest";
    surface.samples.push_back({{{7U, 1.0F}}});
    const auto palette = cook_brick_palette(surface, BrickPaletteCookConfig{});
    const auto records = flat_records();

    BrickPaletteShadingStats stats;
    const auto shaded = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                   BrickPaletteShadingConfig{}, &stats);
    require(shaded.valid, "single slot shading invalid");
    require(shaded.slotsEvaluated == 1U, "single slot evaluated the wrong slot count");
    require_near(shaded.baseColor.x, 0.8F, 1.0e-5F, "single slot base color red wrong");
    require_near(shaded.baseColor.z, 0.1F, 1.0e-5F, "single slot base color blue wrong");
    require_near(shaded.roughness, 0.2F, 1.0e-5F, "single slot roughness wrong");
    require_near(shaded.opacity, 1.0F, 1.0e-5F, "single slot opacity wrong");
    require(shaded.textureSamples == 15U, "single slot triplanar cost wrong");
    require(stats.shadedSamples == 1U && stats.slotEvaluations == 1U, "stats not accumulated");
    require(stats.textureSamples == 15U, "stats texture samples wrong");
    require(stats.unresolvedSlots == 0U, "unexpected unresolved slot");
}

void test_two_slot_channel_blend() {
    const auto palette = two_slot_palette(0.75F);
    const auto records = flat_records();
    const auto shaded = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                   BrickPaletteShadingConfig{});
    require(shaded.valid, "two slot shading invalid");
    require(shaded.slotsEvaluated == 2U, "two slot count wrong");

    const float dominant = brick_palette_sample_weight(palette.samples[0], 0U);
    const float secondary = brick_palette_sample_weight(palette.samples[0], 1U);
    require_near(dominant + secondary, 1.0F, 1.0e-6F, "sample weights do not sum to one");

    require_near(shaded.baseColor.x, 0.8F * dominant, 1.0e-5F, "blended red channel wrong");
    require_near(shaded.baseColor.z, 0.1F * dominant + 0.9F * secondary, 1.0e-5F,
                 "blended blue channel wrong");
    require_near(shaded.roughness, 0.2F * dominant + 0.8F * secondary, 1.0e-5F,
                 "blended roughness wrong");
    require_near(shaded.metallic, 1.0F * secondary, 1.0e-5F, "blended metallic wrong");
    require_near(shaded.opacity, 1.0F * dominant + 0.5F * secondary, 1.0e-5F,
                 "blended opacity wrong");
    require_near(shaded.emissive.x, 0.2F * secondary, 1.0e-5F, "blended emissive wrong");
    require(shaded.textureSamples == 30U, "two slot triplanar cost wrong");
}

void test_blend_is_monotone_in_weight() {
    const auto records = flat_records();
    float previous = -1.0F;
    for (int step = 0; step <= 8; ++step) {
        const float dominantWeight = 0.1F + 0.1F * static_cast<float>(step);
        const auto palette = two_slot_palette(dominantWeight);
        const auto shaded = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                       BrickPaletteShadingConfig{});
        require(shaded.valid, "monotonicity sample invalid");
        require(shaded.baseColor.x >= previous - 1.0e-5F,
                "red channel is not monotone in the dominant weight");
        previous = shaded.baseColor.x;
    }
    require(previous > 0.7F, "high dominant weight did not approach the dominant material");
}

void test_normal_blending() {
    auto records = flat_records();
    records[0].normal = {0.6F, 0.0F, 0.8F};
    records[1].normal = {-0.6F, 0.0F, 0.8F};

    const auto palette = two_slot_palette(0.5F);
    BrickPaletteShadingConfig vectorConfig;
    vectorConfig.blendNormalsAsVectors = true;
    const auto vectorBlend = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                        vectorConfig);
    require(vectorBlend.valid, "vector normal blend invalid");
    require_near(vector_length(vectorBlend.worldNormal), 1.0F, 1.0e-4F,
                 "vector blended normal is not unit length");

    BrickPaletteShadingConfig packedConfig;
    packedConfig.blendNormalsAsVectors = false;
    const auto packedBlend = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                        packedConfig);
    const float packedLength = vector_length(packedBlend.worldNormal);
    require(std::fabs(packedLength - 1.0F) > 0.1F,
            "packed normal averaging did not shorten the normal, so the defect is not measurable");
    require(std::fabs(packedBlend.worldNormal.y - vectorBlend.worldNormal.y) > 0.1F,
            "packed and vector normal blending produced the same result");
}

void test_base_color_blend_space() {
    const auto palette = two_slot_palette(0.5F);
    auto records = flat_records();
    records[0].baseColor = {1.0F, 1.0F, 1.0F};
    records[1].baseColor = {0.0F, 0.0F, 0.0F};

    BrickPaletteShadingConfig linearConfig;
    linearConfig.blendBaseColorInLinearSpace = true;
    const auto linear = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                   linearConfig);

    BrickPaletteShadingConfig encodedConfig;
    encodedConfig.blendBaseColorInLinearSpace = false;
    const auto encoded = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                    encodedConfig);

    require_near(linear.baseColor.x, 0.5F, 5.0e-3F, "linear blend of black and white is not mid");
    require(encoded.baseColor.x < linear.baseColor.x - 0.1F,
            "encoded space blending did not darken relative to linear blending");
}

void test_mapping_modes_agree_on_flat_materials() {
    const auto palette = two_slot_palette(0.6F);
    const auto records = flat_records();

    BrickPaletteShadingConfig triplanar;
    triplanar.mapping = BrickPaletteMappingMode::WorldTriplanar;
    BrickPaletteShadingConfig assetUv;
    assetUv.mapping = BrickPaletteMappingMode::AssetUv;

    const auto a = shade_brick_palette_sample(palette, records, 0U, upward_point(), triplanar);
    const auto b = shade_brick_palette_sample(palette, records, 0U, upward_point(), assetUv);
    require_near(a.baseColor.x, b.baseColor.x, 1.0e-5F,
                 "mapping mode changed a constant material value");
    require_near(a.worldNormal.y, b.worldNormal.y, 1.0e-5F,
                 "mapping mode changed a flat surface normal");
    require(a.textureSamples == b.textureSamples * kBrickPaletteTriplanarProjectionCount,
            "triplanar cost is not the projection multiple of the asset uv cost");
}

void test_detail_modulation_is_position_dependent() {
    auto records = flat_records();
    records[0].detailContrast = 0.5F;
    records[0].textureScale = 1.0F;

    BrickSourceSurface surface;
    surface.brickId = 8U;
    surface.assetName = "DetailTest";
    surface.samples.push_back({{{7U, 1.0F}}});
    const auto palette = cook_brick_palette(surface, BrickPaletteCookConfig{});

    auto first = upward_point();
    auto second = upward_point();
    second.worldPosition = {0.5F, 1.5F, -0.5F};

    const auto a = shade_brick_palette_sample(palette, records, 0U, first,
                                              BrickPaletteShadingConfig{});
    const auto b = shade_brick_palette_sample(palette, records, 0U, second,
                                              BrickPaletteShadingConfig{});
    require(std::fabs(a.baseColor.x - b.baseColor.x) > 1.0e-3F,
            "detail modulation did not vary with world position");
}

void test_unresolved_slots() {
    const auto palette = two_slot_palette(0.75F);
    std::vector<BrickPaletteMaterialRecord> partial;
    partial.push_back(flat_records()[0]);

    BrickPaletteShadingStats stats;
    const auto shaded = shade_brick_palette_sample(palette, partial, 0U, upward_point(),
                                                   BrickPaletteShadingConfig{}, &stats);
    require(shaded.valid, "partial resolution should still shade the surviving slot");
    require(stats.unresolvedSlots == 1U, "unresolved slot not counted");
    require(shaded.slotsEvaluated == 1U, "unresolved slot was still evaluated");
    // Renormalization must keep the surviving material at full strength rather than darkening it.
    require_near(shaded.baseColor.x, 0.8F, 1.0e-5F, "surviving material was not renormalized");
    require(shaded.textureSamples == 15U, "cost did not drop with the unresolved slot");

    std::vector<BrickPaletteMaterialRecord> none;
    BrickPaletteShadingStats emptyStats;
    const auto invalid = shade_brick_palette_sample(palette, none, 0U, upward_point(),
                                                    BrickPaletteShadingConfig{}, &emptyStats);
    require(!invalid.valid, "fully unresolved sample reported as valid");
    require(emptyStats.invalidSamples == 1U, "invalid sample not counted");
    require(invalid.textureSamples == 0U, "invalid sample reported a cost");

    const auto outOfRange = shade_brick_palette_sample(palette, flat_records(), 99U, upward_point(),
                                                       BrickPaletteShadingConfig{});
    require(!outOfRange.valid, "out of range sample index reported as valid");
}

void test_deferred_weight_curve() {
    require_near(brick_palette_deferred_weight(10.0F, 40.0F, 8.0F), 1.0F, 0.0F,
                 "near distance is not fully deferred");
    require_near(brick_palette_deferred_weight(60.0F, 40.0F, 8.0F), 0.0F, 0.0F,
                 "far distance is not fully baked");
    require_near(brick_palette_deferred_weight(40.0F, 40.0F, 8.0F), 0.5F, 1.0e-5F,
                 "midpoint is not an even split");
    require_near(brick_palette_deferred_weight(36.0F, 40.0F, 8.0F), 1.0F, 0.0F,
                 "band start is not fully deferred");
    require_near(brick_palette_deferred_weight(44.0F, 40.0F, 8.0F), 0.0F, 0.0F,
                 "band end is not fully baked");

    float previous = 1.0F;
    for (int step = 0; step <= 200; ++step) {
        const float distance = 30.0F + 0.1F * static_cast<float>(step);
        const float weight = brick_palette_deferred_weight(distance, 40.0F, 8.0F);
        require(weight <= previous + 1.0e-6F, "deferred weight is not monotone non-increasing");
        require(weight >= 0.0F && weight <= 1.0F, "deferred weight left the unit range");
        previous = weight;
    }

    require_near(brick_palette_deferred_weight(39.9F, 40.0F, 0.0F), 1.0F, 0.0F,
                 "zero band did not behave as a hard cut");
    require_near(brick_palette_deferred_weight(40.1F, 40.0F, 0.0F), 0.0F, 0.0F,
                 "zero band did not behave as a hard cut past the boundary");
}

void test_lod_transition_blend() {
    const auto palette = two_slot_palette(0.75F);
    auto records = flat_records();
    const auto deferred = shade_brick_palette_sample(palette, records, 0U, upward_point(),
                                                     BrickPaletteShadingConfig{});

    records[0].baseColor = {0.2F, 0.2F, 0.2F};
    records[1].baseColor = {0.2F, 0.2F, 0.2F};
    const auto baked = bake_brick_palette_sample(palette, records, 0U, upward_point(),
                                                 BrickPaletteShadingConfig{});
    require(baked.valid, "baked reference invalid");
    require(baked.textureSamples == 1U, "baked reference did not collapse to one fetch");

    const auto atZero = blend_brick_palette_samples(deferred, baked, 0.0F);
    const auto atOne = blend_brick_palette_samples(deferred, baked, 1.0F);
    require_near(atZero.baseColor.x, baked.baseColor.x, 0.0F, "weight zero is not exactly baked");
    require_near(atOne.baseColor.x, deferred.baseColor.x, 0.0F,
                 "weight one is not exactly deferred");

    const auto nearZero = blend_brick_palette_samples(deferred, baked, 1.0e-5F);
    require_near(nearZero.baseColor.x, baked.baseColor.x, 1.0e-4F,
                 "transition is discontinuous at the baked end");
    const auto nearOne = blend_brick_palette_samples(deferred, baked, 1.0F - 1.0e-5F);
    require_near(nearOne.baseColor.x, deferred.baseColor.x, 1.0e-4F,
                 "transition is discontinuous at the deferred end");

    float previous = baked.baseColor.x;
    for (int step = 0; step <= 100; ++step) {
        const float weight = static_cast<float>(step) / 100.0F;
        const auto blended = blend_brick_palette_samples(deferred, baked, weight);
        require(blended.valid, "blended sample invalid");
        require(blended.baseColor.x >= previous - 1.0e-5F,
                "transition is not monotone toward the deferred value");
        require_near(vector_length(blended.worldNormal), 1.0F, 1.0e-4F,
                     "blended normal is not unit length");
        previous = blended.baseColor.x;
    }
}

void test_baked_matches_deferred_value() {
    const auto palette = two_slot_palette(0.6F);
    const auto records = flat_records();
    const auto point = upward_point();
    const BrickPaletteShadingConfig config;

    const auto deferred = shade_brick_palette_sample(palette, records, 0U, point, config);
    const auto baked = bake_brick_palette_sample(palette, records, 0U, point, config);
    require_near(baked.baseColor.x, deferred.baseColor.x, 0.0F,
                 "baked value diverges from the deferred value at the same point");
    require_near(baked.roughness, deferred.roughness, 0.0F, "baked roughness diverges");
    require(baked.textureSamples < deferred.textureSamples,
            "baked path is not cheaper than the deferred path");
}

void test_determinism() {
    const auto palette = two_slot_palette(0.65F);
    const auto records = flat_records();
    const auto point = upward_point();
    const BrickPaletteShadingConfig config;

    const auto first = shade_brick_palette_sample(palette, records, 0U, point, config);
    for (int repeat = 0; repeat < 16; ++repeat) {
        const auto again = shade_brick_palette_sample(palette, records, 0U, point, config);
        require_near(again.baseColor.x, first.baseColor.x, 0.0F, "shading is not deterministic");
        require_near(again.worldNormal.y, first.worldNormal.y, 0.0F,
                     "normal shading is not deterministic");
        require(again.textureSamples == first.textureSamples, "cost is not deterministic");
    }
}

} // namespace

int main() {
    try {
        test_texture_sample_counters();
        test_single_slot_matches_source_material();
        test_two_slot_channel_blend();
        test_blend_is_monotone_in_weight();
        test_normal_blending();
        test_base_color_blend_space();
        test_mapping_modes_agree_on_flat_materials();
        test_detail_modulation_is_position_dependent();
        test_unresolved_slots();
        test_deferred_weight_curve();
        test_lod_transition_blend();
        test_baked_matches_deferred_value();
        test_determinism();
    } catch (const std::exception& error) {
        std::cerr << "brick palette shading v2.25 test failure: " << error.what() << "\n";
        return 1;
    }
    std::cout << "brick palette shading v2.25 tests passed\n";
    return 0;
}
