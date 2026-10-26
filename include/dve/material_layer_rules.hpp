#pragma once

// Rule-driven layer masks: where a material layer (VoxelMaterialLayer) covers the surface, from
// the geometry instead of a painted mask. Snow on what faces up, moss in hollows, dirt below a
// height, wear along exposed edges, rust where the base height map is low, all broken up by
// noise.
//
// A layer's coverage is weight x material_layer_rule_factor(rules, context). Rules that are not
// enabled give factor 1, which is the plain uniform-weight layer as before.
//
// Each rule is a soft band. The transition is centred on its threshold (coverage 0.5 exactly
// there) and is `softness` wide (`heightSoftnessMeters` for height). The factors multiply.
//
//   slope      cos(angle between the world normal and `up`) within [slopeMinCos, slopeMaxCos]:
//              1 faces up, 0 vertical, -1 faces down. Snow: slopeMinCos 0.7.
//   height     world height along `up`, in metres, within [heightMinMeters, heightMaxMeters].
//   occlusion  ambient occlusion (1 open, 0 enclosed) within [occlusionMin, occlusionMax].
//              Dirt in hollows: occlusionMax 0.75.
//   edge       edgeWidthMeters > 0: only within that distance of an exposed convex voxel edge
//              (where two exposed faces meet). Surfaces with no edge information (meshes) get 0.
//
// Then two shifts s are summed and scale it: coverage = clamp(factor x (1 + s), 0, 1), so they
// break up and bias the bands but never reach outside them:
//   base height  baseHeightInfluence x (1 - 2h), with h the base material's height (its height
//                map or surface pattern; 0.5 when it has none). Positive values fill low parts
//                first (water and dirt in cracks); negative values favour high parts (wear on
//                tops).
//   noise        noiseAmount x n, with n a lattice noise of about [-1, 1] and period
//                noiseScaleMeters (seeded). It gives ragged, natural boundaries.

#include <array>
#include <cstdint>
#include <limits>

#include "dve/types.hpp"

namespace dve {

struct MaterialLayerRules {
    bool enabled{false};
    std::uint32_t seed{1};
    Float3 up{0.0F, 1.0F, 0.0F};
    float slopeMinCos{-1.0F};
    float slopeMaxCos{1.0F};
    float heightMinMeters{-1.0e9F};
    float heightMaxMeters{1.0e9F};
    float occlusionMin{0.0F};
    float occlusionMax{1.0F};
    float edgeWidthMeters{0.0F};
    float baseHeightInfluence{0.0F};
    float noiseScaleMeters{1.0F};
    float noiseAmount{0.0F};
    float softness{0.1F};
    float heightSoftnessMeters{0.5F};
};

inline constexpr std::size_t kMaterialLayerRuleFloatCount = 15;
[[nodiscard]] std::array<float, kMaterialLayerRuleFloatCount> material_layer_rule_values(const MaterialLayerRules&) noexcept;
void set_material_layer_rule_values(MaterialLayerRules&, const std::array<float, kMaterialLayerRuleFloatCount>&) noexcept;
[[nodiscard]] bool material_layer_rules_valid(const MaterialLayerRules&) noexcept;
[[nodiscard]] std::uint64_t material_layer_rules_hash(const MaterialLayerRules&) noexcept;   // 0 when disabled

struct MaterialLayerRuleContext {
    Float3 worldPositionMeters{};
    Float3 worldNormal{0.0F, 1.0F, 0.0F};
    float ambientOcclusion{1.0F};
    float edgeDistanceMeters{std::numeric_limits<float>::infinity()};   // infinity: no edge information
    float baseHeight{0.5F};
    float footprintMeters{0.0F};   // filters the noise
};

// The rules' coverage factor in [0, 1]: 1 when the rules are disabled or invalid.
[[nodiscard]] float material_layer_rule_factor(const MaterialLayerRules&, const MaterialLayerRuleContext&) noexcept;

// Distance from a point in a voxel to its nearest exposed convex edge, in the voxel's own units:
// `local` is the point in [0, 1]^3 of the cell, `exposedFaces` bits 1 -x, 2 +x, 4 -y, 8 +y,
// 16 -z, 32 +z. Infinity when no two exposed faces meet.
[[nodiscard]] float voxel_exposed_edge_distance(Float3 local, std::uint8_t exposedFaces) noexcept;

} // namespace dve
