#include "dve/render/brick_palette_shading.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace dve::render {
namespace {

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] float clamp01(float value) noexcept {
    if (!(value > 0.0F)) return 0.0F;
    if (value > 1.0F) return 1.0F;
    return value;
}

[[nodiscard]] BrickPaletteFloat3 add(BrickPaletteFloat3 left, BrickPaletteFloat3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] BrickPaletteFloat3 scale(BrickPaletteFloat3 value, float factor) noexcept {
    return {value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] float length(BrickPaletteFloat3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] BrickPaletteFloat3 normalize_or(BrickPaletteFloat3 value,
                                              BrickPaletteFloat3 fallback) noexcept {
    const float magnitude = length(value);
    if (!(magnitude > 1.0e-8F)) return fallback;
    return scale(value, 1.0F / magnitude);
}

[[nodiscard]] float encode_srgb(float linear) noexcept {
    const float clamped = clamp01(linear);
    if (clamped <= 0.0031308F) return clamped * 12.92F;
    return 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float decode_srgb(float encoded) noexcept {
    const float clamped = clamp01(encoded);
    if (clamped <= 0.04045F) return clamped / 12.92F;
    return std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

// Analytic stand-in for an authored texture set. Deterministic and free of image dependencies.
[[nodiscard]] float detail_factor(const BrickPaletteMaterialRecord& record, float u, float v) noexcept {
    if (!(record.detailContrast > 0.0F)) return 1.0F;
    const float pattern = 0.5F + 0.5F * std::sin(u * kPi) * std::sin(v * kPi);
    const float factor = 1.0F + record.detailContrast * (pattern * 2.0F - 1.0F);
    return factor > 0.0F ? factor : 0.0F;
}

struct Projection {
    float weight{};
    float u{};
    float v{};
    // World-space basis columns for converting a tangent-space normal.
    BrickPaletteFloat3 tangent{};
    BrickPaletteFloat3 bitangent{};
    BrickPaletteFloat3 normal{};
};

// Three axis-aligned projections with a sharpness-weighted blend, plus a single-projection asset UV
// mode. Both return a normalized weight set so downstream blending is mapping agnostic.
[[nodiscard]] std::array<Projection, kBrickPaletteTriplanarProjectionCount> build_projections(
    const BrickPaletteSurfacePoint& point, const BrickPaletteShadingConfig& config,
    float textureScale) noexcept {
    std::array<Projection, kBrickPaletteTriplanarProjectionCount> projections{};
    const BrickPaletteFloat3 surfaceNormal =
        normalize_or(point.worldNormal, BrickPaletteFloat3{0.0F, 1.0F, 0.0F});
    const float scaleFactor = textureScale > 0.0F ? textureScale : 1.0F;

    if (config.mapping == BrickPaletteMappingMode::AssetUv) {
        projections[0].weight = 1.0F;
        projections[0].u = point.assetU * scaleFactor;
        projections[0].v = point.assetV * scaleFactor;
        projections[0].tangent = {1.0F, 0.0F, 0.0F};
        projections[0].bitangent = {0.0F, 1.0F, 0.0F};
        projections[0].normal = surfaceNormal;
        return projections;
    }

    const float sharpness = config.triplanarSharpness > 0.0F ? config.triplanarSharpness : 1.0F;
    const float ax = std::pow(std::fabs(surfaceNormal.x), sharpness);
    const float ay = std::pow(std::fabs(surfaceNormal.y), sharpness);
    const float az = std::pow(std::fabs(surfaceNormal.z), sharpness);
    const float total = ax + ay + az;
    const float inverse = total > 1.0e-8F ? 1.0F / total : 0.0F;

    // X projection samples the YZ plane.
    projections[0].weight = total > 1.0e-8F ? ax * inverse : 1.0F;
    projections[0].u = point.worldPosition.y * scaleFactor;
    projections[0].v = point.worldPosition.z * scaleFactor;
    projections[0].tangent = {0.0F, 1.0F, 0.0F};
    projections[0].bitangent = {0.0F, 0.0F, 1.0F};
    projections[0].normal = {surfaceNormal.x >= 0.0F ? 1.0F : -1.0F, 0.0F, 0.0F};

    // Y projection samples the XZ plane.
    projections[1].weight = total > 1.0e-8F ? ay * inverse : 0.0F;
    projections[1].u = point.worldPosition.z * scaleFactor;
    projections[1].v = point.worldPosition.x * scaleFactor;
    projections[1].tangent = {0.0F, 0.0F, 1.0F};
    projections[1].bitangent = {1.0F, 0.0F, 0.0F};
    projections[1].normal = {0.0F, surfaceNormal.y >= 0.0F ? 1.0F : -1.0F, 0.0F};

    // Z projection samples the XY plane.
    projections[2].weight = total > 1.0e-8F ? az * inverse : 0.0F;
    projections[2].u = point.worldPosition.x * scaleFactor;
    projections[2].v = point.worldPosition.y * scaleFactor;
    projections[2].tangent = {1.0F, 0.0F, 0.0F};
    projections[2].bitangent = {0.0F, 1.0F, 0.0F};
    projections[2].normal = {0.0F, 0.0F, surfaceNormal.z >= 0.0F ? 1.0F : -1.0F};
    return projections;
}

struct EvaluatedMaterial {
    BrickPaletteFloat3 baseColor{};
    float roughness{};
    float metallic{};
    BrickPaletteFloat3 emissive{};
    float opacity{};
    BrickPaletteFloat3 worldNormal{};
};

[[nodiscard]] EvaluatedMaterial evaluate_material(
    const BrickPaletteMaterialRecord& record, const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config) noexcept {
    const auto projections = build_projections(point, config, record.textureScale);
    const BrickPaletteFloat3 tangentNormal =
        normalize_or(record.normal, BrickPaletteFloat3{0.0F, 0.0F, 1.0F});

    EvaluatedMaterial result;
    BrickPaletteFloat3 accumulatedNormal{};
    for (const auto& projection : projections) {
        if (!(projection.weight > 0.0F)) continue;
        const float factor = detail_factor(record, projection.u, projection.v);
        result.baseColor = add(result.baseColor,
                               scale(record.baseColor, projection.weight * factor));
        result.roughness += clamp01(record.roughness * factor) * projection.weight;
        result.metallic += clamp01(record.metallic) * projection.weight;
        result.emissive = add(result.emissive, scale(record.emissive, projection.weight * factor));
        result.opacity += clamp01(record.opacity) * projection.weight;

        const BrickPaletteFloat3 rotated = add(
            add(scale(projection.tangent, tangentNormal.x),
                scale(projection.bitangent, tangentNormal.y)),
            scale(projection.normal, tangentNormal.z));
        accumulatedNormal = add(accumulatedNormal, scale(rotated, projection.weight));
    }
    result.worldNormal = normalize_or(accumulatedNormal,
                                      normalize_or(point.worldNormal,
                                                   BrickPaletteFloat3{0.0F, 1.0F, 0.0F}));
    return result;
}

} // namespace

const char* to_string(BrickPaletteMappingMode value) noexcept {
    switch (value) {
        case BrickPaletteMappingMode::WorldTriplanar: return "world_triplanar";
        case BrickPaletteMappingMode::AssetUv: return "asset_uv";
    }
    return "unknown";
}

const BrickPaletteMaterialRecord* find_brick_palette_material(
    std::span<const BrickPaletteMaterialRecord> records, std::uint32_t globalMaterialId) noexcept {
    for (const auto& record : records)
        if (record.globalMaterialId == globalMaterialId) return &record;
    return nullptr;
}

std::uint32_t brick_palette_texture_sample_count(
    std::uint8_t slotsEvaluated, BrickPaletteMappingMode mapping) noexcept {
    const std::uint32_t projections = mapping == BrickPaletteMappingMode::WorldTriplanar
                                          ? kBrickPaletteTriplanarProjectionCount
                                          : 1U;
    return static_cast<std::uint32_t>(slotsEvaluated) * kBrickPaletteShadingChannelCount *
           projections;
}

BrickPaletteShadedSample shade_brick_palette_encoding(
    const CookedBrickPalette& palette,
    std::span<const BrickPaletteMaterialRecord> records,
    const BrickPaletteSampleEncoding& encoding,
    const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config,
    BrickPaletteShadingStats* stats) {
    BrickPaletteShadedSample result;
    result.worldNormal = normalize_or(point.worldNormal, BrickPaletteFloat3{0.0F, 1.0F, 0.0F});

    BrickPaletteFloat3 accumulatedColor{};
    BrickPaletteFloat3 accumulatedNormal{};
    BrickPaletteFloat3 packedNormal{};
    float accumulatedWeight = 0.0F;
    std::uint8_t evaluated = 0U;

    for (std::uint8_t index = 0U; index < encoding.usedSlots; ++index) {
        if (index >= kBrickPaletteMaximumSlots) break;
        const std::uint8_t slot = encoding.slotIndices[index];
        if (slot >= palette.slots.size()) {
            if (stats != nullptr) ++stats->unresolvedSlots;
            continue;
        }
        const BrickPaletteMaterialRecord* record =
            find_brick_palette_material(records, palette.slots[slot]);
        if (record == nullptr) {
            if (stats != nullptr) ++stats->unresolvedSlots;
            continue;
        }
        const float weight = brick_palette_sample_weight(encoding, index);
        if (!(weight > 0.0F)) continue;

        const EvaluatedMaterial material = evaluate_material(*record, point, config);
        if (config.blendBaseColorInLinearSpace) {
            accumulatedColor = add(accumulatedColor, scale(material.baseColor, weight));
        } else {
            accumulatedColor.x += encode_srgb(material.baseColor.x) * weight;
            accumulatedColor.y += encode_srgb(material.baseColor.y) * weight;
            accumulatedColor.z += encode_srgb(material.baseColor.z) * weight;
        }
        result.roughness += material.roughness * weight;
        result.metallic += material.metallic * weight;
        result.emissive = add(result.emissive, scale(material.emissive, weight));
        result.opacity += material.opacity * weight;

        accumulatedNormal = add(accumulatedNormal, scale(material.worldNormal, weight));
        // Packed-average defect path: encode to [0,1] first, average, decode afterwards.
        packedNormal.x += (material.worldNormal.x * 0.5F + 0.5F) * weight;
        packedNormal.y += (material.worldNormal.y * 0.5F + 0.5F) * weight;
        packedNormal.z += (material.worldNormal.z * 0.5F + 0.5F) * weight;

        accumulatedWeight += weight;
        ++evaluated;
        if (stats != nullptr) ++stats->slotEvaluations;
    }

    if (evaluated == 0U || !(accumulatedWeight > 0.0F)) {
        result.valid = false;
        result.slotsEvaluated = 0U;
        result.textureSamples = 0U;
        if (stats != nullptr) {
            ++stats->shadedSamples;
            ++stats->invalidSamples;
        }
        return result;
    }

    // Quantized weights sum to one by construction, but a dropped or unresolved slot can leave a
    // partial sum. Renormalizing keeps energy conserved instead of darkening the surface.
    const float inverse = 1.0F / accumulatedWeight;
    result.roughness = clamp01(result.roughness * inverse);
    result.metallic = clamp01(result.metallic * inverse);
    result.emissive = scale(result.emissive, inverse);
    result.opacity = clamp01(result.opacity * inverse);

    accumulatedColor = scale(accumulatedColor, inverse);
    if (config.blendBaseColorInLinearSpace) {
        result.baseColor = accumulatedColor;
    } else {
        result.baseColor = {decode_srgb(accumulatedColor.x), decode_srgb(accumulatedColor.y),
                            decode_srgb(accumulatedColor.z)};
    }

    if (config.blendNormalsAsVectors) {
        result.worldNormal = normalize_or(scale(accumulatedNormal, inverse), result.worldNormal);
    } else {
        const BrickPaletteFloat3 averaged = scale(packedNormal, inverse);
        result.worldNormal = {averaged.x * 2.0F - 1.0F, averaged.y * 2.0F - 1.0F,
                              averaged.z * 2.0F - 1.0F};
    }

    result.slotsEvaluated = evaluated;
    result.textureSamples = brick_palette_texture_sample_count(evaluated, config.mapping);
    result.valid = true;
    if (stats != nullptr) {
        ++stats->shadedSamples;
        stats->textureSamples += result.textureSamples;
    }
    return result;
}

BrickPaletteShadedSample shade_brick_palette_sample(
    const CookedBrickPalette& palette,
    std::span<const BrickPaletteMaterialRecord> records,
    std::size_t sampleIndex,
    const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config,
    BrickPaletteShadingStats* stats) {
    if (sampleIndex >= palette.samples.size()) {
        BrickPaletteShadedSample invalid;
        invalid.worldNormal = normalize_or(point.worldNormal, BrickPaletteFloat3{0.0F, 1.0F, 0.0F});
        if (stats != nullptr) {
            ++stats->shadedSamples;
            ++stats->invalidSamples;
        }
        return invalid;
    }
    return shade_brick_palette_encoding(palette, records, palette.samples[sampleIndex], point,
                                        config, stats);
}

BrickPaletteShadedSample bake_brick_palette_sample(
    const CookedBrickPalette& palette,
    std::span<const BrickPaletteMaterialRecord> records,
    std::size_t sampleIndex,
    const BrickPaletteSurfacePoint& point,
    const BrickPaletteShadingConfig& config,
    BrickPaletteShadingStats* stats) {
    // The baked path resolves the same blend once at cook time and stores the result, so the value
    // matches the deferred result while the runtime cost collapses to a single property fetch.
    BrickPaletteShadedSample baked =
        shade_brick_palette_sample(palette, records, sampleIndex, point, config, nullptr);
    baked.textureSamples = baked.valid ? 1U : 0U;
    baked.slotsEvaluated = baked.valid ? 1U : 0U;
    if (stats != nullptr) {
        ++stats->shadedSamples;
        stats->textureSamples += baked.textureSamples;
        if (!baked.valid) ++stats->invalidSamples;
    }
    return baked;
}

float brick_palette_deferred_weight(
    float cameraDistance, float deferredMaximumDistance, float transitionBand) noexcept {
    if (!(transitionBand > 0.0F))
        return cameraDistance <= deferredMaximumDistance ? 1.0F : 0.0F;
    const float start = deferredMaximumDistance - transitionBand * 0.5F;
    const float end = deferredMaximumDistance + transitionBand * 0.5F;
    if (cameraDistance <= start) return 1.0F;
    if (cameraDistance >= end) return 0.0F;
    const float t = (cameraDistance - start) / (end - start);
    return 1.0F - t * t * (3.0F - 2.0F * t);
}

BrickPaletteShadedSample blend_brick_palette_samples(
    const BrickPaletteShadedSample& deferred,
    const BrickPaletteShadedSample& baked,
    float deferredWeight) {
    if (!(deferredWeight > 0.0F)) return baked;
    if (deferredWeight >= 1.0F) return deferred;
    if (!deferred.valid) return baked;
    if (!baked.valid) return deferred;

    const float a = deferredWeight;
    const float b = 1.0F - deferredWeight;
    BrickPaletteShadedSample result;
    result.baseColor = add(scale(deferred.baseColor, a), scale(baked.baseColor, b));
    result.roughness = deferred.roughness * a + baked.roughness * b;
    result.metallic = deferred.metallic * a + baked.metallic * b;
    result.emissive = add(scale(deferred.emissive, a), scale(baked.emissive, b));
    result.opacity = deferred.opacity * a + baked.opacity * b;
    result.worldNormal = normalize_or(
        add(scale(deferred.worldNormal, a), scale(baked.worldNormal, b)), deferred.worldNormal);
    result.slotsEvaluated = std::max(deferred.slotsEvaluated, baked.slotsEvaluated);
    result.textureSamples = deferred.textureSamples + baked.textureSamples;
    result.valid = true;
    return result;
}

} // namespace dve::render
