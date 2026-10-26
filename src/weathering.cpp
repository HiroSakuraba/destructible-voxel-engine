#include "dve/weathering.hpp"

#include <algorithm>

namespace dve {

const char* weathering_kind_label(WeatheringKind kind) noexcept {
    switch (kind) {
    case WeatheringKind::Snow: return "Snow";
    case WeatheringKind::Moss: return "Moss";
    case WeatheringKind::Rust: return "Rust";
    case WeatheringKind::Dust: return "Dust";
    case WeatheringKind::Wetness: return "Wetness";
    case WeatheringKind::EdgeWear: return "Edge wear";
    }
    return "Unknown";
}

WeatheringLayer make_weathering_layer(WeatheringKind kind, float amount, Float3 up, std::uint32_t seed) noexcept {
    const float a = std::clamp(amount, 0.0F, 1.0F);
    const auto lerp = [&](float from, float to) { return from + (to - from) * a; };
    WeatheringLayer out;
    VoxelMaterialDefinition& s = out.source;
    VoxelMaterialLayer& l = out.layer;
    MaterialLayerRules& r = l.rules;
    l.weight = 1.0F;
    r.enabled = true;
    r.seed = seed;
    r.up = up;
    s.specular = 0.5F;
    switch (kind) {
    case WeatheringKind::Snow:
        s.name = "Snow";
        s.baseColor = {0.9F, 0.92F, 0.95F, 1.0F};
        s.roughness = 0.75F;
        s.subsurfaceColor = {0.8F, 0.88F, 1.0F};
        r.slopeMinCos = lerp(0.95F, 0.35F);
        r.softness = 0.15F;
        r.baseHeightInfluence = 0.3F;
        r.noiseAmount = 0.25F;
        r.noiseScaleMeters = 1.5F;
        break;
    case WeatheringKind::Moss:
        s.name = "Moss";
        s.baseColor = {0.12F, 0.24F, 0.06F, 1.0F};
        s.roughness = 0.95F;
        s.specular = 0.2F;
        r.occlusionMax = lerp(0.35F, 0.95F);
        r.slopeMinCos = -0.2F;
        r.softness = 0.15F;
        r.baseHeightInfluence = 0.5F;
        r.noiseAmount = 0.35F;
        r.noiseScaleMeters = 0.6F;
        break;
    case WeatheringKind::Rust:
        s.name = "Rust";
        s.baseColor = {0.33F, 0.12F, 0.04F, 1.0F};
        s.roughness = 0.9F;
        s.metallic = 0.0F;
        r.edgeWidthMeters = lerp(0.005F, 0.08F);
        r.baseHeightInfluence = 0.4F;
        r.noiseAmount = 0.4F;
        r.noiseScaleMeters = 0.15F;
        break;
    case WeatheringKind::Dust:
        s.name = "Dust";
        s.baseColor = {0.55F, 0.5F, 0.42F, 1.0F};
        s.roughness = 1.0F;
        s.specular = 0.3F;
        l.weight = lerp(0.2F, 0.9F);
        r.slopeMinCos = lerp(0.9F, 0.3F);
        r.softness = 0.2F;
        r.baseHeightInfluence = 0.6F;
        r.noiseAmount = 0.2F;
        r.noiseScaleMeters = 0.8F;
        break;
    case WeatheringKind::Wetness:
        s.name = "Wetness";
        s.baseColor = {0.55F, 0.55F, 0.55F, 1.0F};
        s.roughness = 0.15F;
        s.specular = 1.0F;
        s.metallic = 1.0F;   // multiply: keeps the base's metal
        l.blendMode = MaterialLayerBlendMode::Multiply;
        l.weight = lerp(0.3F, 1.0F);
        r.slopeMinCos = 0.85F;
        r.softness = 0.1F;
        r.baseHeightInfluence = 1.0F;
        r.noiseAmount = 0.15F;
        r.noiseScaleMeters = 2.0F;
        break;
    case WeatheringKind::EdgeWear:
        s.name = "Edge wear";
        s.baseColor = {0.62F, 0.6F, 0.56F, 1.0F};
        s.roughness = 0.8F;
        r.edgeWidthMeters = lerp(0.002F, 0.03F);
        r.noiseAmount = 0.5F;
        r.noiseScaleMeters = 0.05F;
        break;
    }
    return out;
}

bool add_weathering(std::vector<VoxelMaterialDefinition>& materials, MaterialId target, WeatheringKind kind, float amount, Float3 up,
                    std::uint32_t seed, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (target >= materials.size()) return fail("weathering target material is out of range");
    if (materials[target].layers.size() >= kMaximumVoxelMaterialLayers) return fail("weathering target already has four layers");
    if (materials.size() >= 256U) return fail("material table is full");
    WeatheringLayer recipe = make_weathering_layer(kind, amount, up, seed);
    recipe.layer.sourceMaterial = static_cast<MaterialId>(materials.size());
    materials.push_back(std::move(recipe.source));
    materials[target].layers.push_back(recipe.layer);
    return true;
}

} // namespace dve
