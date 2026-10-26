#include "dve/material_layer_rules.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "dve/asset_cooker.hpp"
#include "dve/material_surface_detail.hpp"
#include "dve/transform.hpp"

namespace dve {

std::array<float, kMaterialLayerRuleFloatCount> material_layer_rule_values(const MaterialLayerRules& r) noexcept {
    return {r.up.x,          r.up.y,           r.up.z,         r.slopeMinCos,         r.slopeMaxCos,
            r.heightMinMeters, r.heightMaxMeters, r.occlusionMin, r.occlusionMax,       r.edgeWidthMeters,
            r.baseHeightInfluence, r.noiseScaleMeters, r.noiseAmount, r.softness, r.heightSoftnessMeters};
}

void set_material_layer_rule_values(MaterialLayerRules& r, const std::array<float, kMaterialLayerRuleFloatCount>& v) noexcept {
    r.up = {v[0], v[1], v[2]};
    r.slopeMinCos = v[3];
    r.slopeMaxCos = v[4];
    r.heightMinMeters = v[5];
    r.heightMaxMeters = v[6];
    r.occlusionMin = v[7];
    r.occlusionMax = v[8];
    r.edgeWidthMeters = v[9];
    r.baseHeightInfluence = v[10];
    r.noiseScaleMeters = v[11];
    r.noiseAmount = v[12];
    r.softness = v[13];
    r.heightSoftnessMeters = v[14];
}

bool material_layer_rules_valid(const MaterialLayerRules& r) noexcept {
    for (const float v : material_layer_rule_values(r))
        if (!std::isfinite(v)) return false;
    const float upLength = std::sqrt(r.up.x * r.up.x + r.up.y * r.up.y + r.up.z * r.up.z);
    return upLength > 1.0e-6F && r.slopeMinCos >= -1.0F && r.slopeMaxCos <= 1.0F && r.slopeMinCos <= r.slopeMaxCos &&
           r.heightMinMeters >= -1.0e9F && r.heightMaxMeters <= 1.0e9F && r.heightMinMeters <= r.heightMaxMeters &&
           r.occlusionMin >= 0.0F && r.occlusionMax <= 1.0F && r.occlusionMin <= r.occlusionMax && r.edgeWidthMeters >= 0.0F &&
           r.edgeWidthMeters <= 100.0F && r.baseHeightInfluence >= -1.0F && r.baseHeightInfluence <= 1.0F &&
           r.noiseScaleMeters >= 1.0e-4F && r.noiseScaleMeters <= 1.0e6F && r.noiseAmount >= 0.0F && r.noiseAmount <= 1.0F &&
           r.softness >= 0.0F && r.softness <= 1.0F && r.heightSoftnessMeters >= 0.0F && r.heightSoftnessMeters <= 1.0e6F;
}

std::uint64_t material_layer_rules_hash(const MaterialLayerRules& r) noexcept {
    if (!r.enabled) return 0U;
    std::uint64_t h = 1469598103934665603ULL;
    const auto add = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 32; i += 8) h = (h ^ ((value >> i) & 255U)) * 1099511628211ULL;
    };
    add(0x72756c65U);   // "rule"
    add(r.seed);
    for (const float v : material_layer_rule_values(r)) add(std::bit_cast<std::uint32_t>(v));
    return h;
}

namespace {
// 0 below the band, 1 inside, with transitions of width `soft` centred on each threshold. An
// end at the domain's limit (or beyond) has no transition.
float band(float x, float lo, float hi, float soft, float domainLo, float domainHi) noexcept {
    const auto step = [&](float edge, float t) {
        if (soft <= 0.0F) return t >= edge ? 1.0F : 0.0F;
        const float s = std::clamp((t - edge) / soft + 0.5F, 0.0F, 1.0F);
        return s * s * (3.0F - 2.0F * s);
    };
    const float lower = lo <= domainLo ? 1.0F : step(lo, x);
    const float upper = hi >= domainHi ? 1.0F : 1.0F - step(hi, x);
    return lower * upper;
}
} // namespace

float material_layer_rule_factor(const MaterialLayerRules& r, const MaterialLayerRuleContext& c) noexcept {
    if (!r.enabled || !material_layer_rules_valid(r)) return 1.0F;
    const Float3 up = normalize(r.up);
    float factor = 1.0F;
    const float n = std::sqrt(dot(c.worldNormal, c.worldNormal));
    const float cosine = n > 0.0F ? dot(c.worldNormal, up) / n : 0.0F;
    factor *= band(cosine, r.slopeMinCos, r.slopeMaxCos, r.softness, -1.0F, 1.0F);
    factor *= band(dot(c.worldPositionMeters, up), r.heightMinMeters, r.heightMaxMeters, r.heightSoftnessMeters, -1.0e9F, 1.0e9F);
    factor *= band(std::clamp(c.ambientOcclusion, 0.0F, 1.0F), r.occlusionMin, r.occlusionMax, r.softness, 0.0F, 1.0F);
    if (r.edgeWidthMeters > 0.0F) {
        if (!std::isfinite(c.edgeDistanceMeters)) return 0.0F;
        const float s = std::clamp(1.0F - c.edgeDistanceMeters / r.edgeWidthMeters, 0.0F, 1.0F);
        factor *= s * s * (3.0F - 2.0F * s);
    }
    float shift = r.baseHeightInfluence * (1.0F - 2.0F * std::clamp(c.baseHeight, 0.0F, 1.0F));
    if (r.noiseAmount > 0.0F) {
        MaterialSurfaceDetail noise;
        noise.pattern = SurfacePattern::Mineral;
        noise.periodMeters = r.noiseScaleMeters;
        noise.seed = r.seed;
        noise.bumpHeightMeters = 0.0F;
        shift += r.noiseAmount * sample_surface_pattern(noise, c.worldPositionMeters, c.footprintMeters).value;
    }
    return std::clamp(factor * (1.0F + shift), 0.0F, 1.0F);
}

std::uint64_t material_rules_hash(const VoxelMaterialDefinition& m) noexcept {
    if (!material_has_rules(m)) return 0U;
    std::uint64_t h = 1469598103934665603ULL;
    const auto add = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 64; i += 8) h = (h ^ ((value >> i) & 255U)) * 1099511628211ULL;
    };
    add(std::bit_cast<std::uint32_t>(m.textureAntiTiling));
    add(m.layers.size());
    for (const VoxelMaterialLayer& layer : m.layers) add(material_layer_rules_hash(layer.rules));
    return h;
}

float voxel_exposed_edge_distance(Float3 local, std::uint8_t exposedFaces) noexcept {
    const float p[3] = {local.x, local.y, local.z};
    float best = std::numeric_limits<float>::infinity();
    for (int a = 0; a < 6; ++a) {
        if ((exposedFaces & (1U << a)) == 0U) continue;
        for (int b = a + 1; b < 6; ++b) {
            if ((exposedFaces & (1U << b)) == 0U || a / 2 == b / 2) continue;
            const float da = (a & 1) ? 1.0F - p[a / 2] : p[a / 2];
            const float db = (b & 1) ? 1.0F - p[b / 2] : p[b / 2];
            best = std::min(best, std::sqrt(std::max(da, 0.0F) * std::max(da, 0.0F) + std::max(db, 0.0F) * std::max(db, 0.0F)));
        }
    }
    return best;
}

} // namespace dve
