#pragma once

// Weathering recipes: ready-made rule-driven layers (dve/material_layer_rules.hpp) for the usual
// kinds of wear. `amount` is the stage, from 0 (barely started) to 1 (fully weathered). It widens
// the rule bands, so the same material can age in place (snow deepening, moss spreading, rust
// creeping along edges).
//
//   Snow      white and rough on what faces up. The slope band opens from cos 0.95 to cos 0.35;
//             it settles in hollows first.
//   Moss      green and soft in occluded places. The occlusion band opens from 0.35 to 0.95; it
//             grows on tops and sides, not undersides, starts low and has ragged edges.
//   Rust      orange-brown and rough along exposed edges, 0.5 cm to 8 cm wide, and in pits.
//   Dust      pale and matte on near-flat surfaces, collecting in the low parts.
//   Wetness   darker and glossy (a multiply layer), pooling in the low parts of flat surfaces.
//   EdgeWear  the paint rubbed off exposed edges: lighter and rougher, 2 mm to 3 cm wide.

#include <cstdint>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"

namespace dve {

enum class WeatheringKind : std::uint8_t { Snow, Moss, Rust, Dust, Wetness, EdgeWear };

[[nodiscard]] const char* weathering_kind_label(WeatheringKind kind) noexcept;

struct WeatheringLayer {
    VoxelMaterialDefinition source;   // the layer's material
    VoxelMaterialLayer layer;         // sourceMaterial is left for the caller to set
};

[[nodiscard]] WeatheringLayer make_weathering_layer(WeatheringKind kind, float amount, Float3 up = {0.0F, 1.0F, 0.0F},
                                                    std::uint32_t seed = 1U) noexcept;

// Appends the recipe's source material to `materials` and its layer to materials[target].
// Fails, with *error and nothing changed, when the target is out of range, already has four
// layers, or the table already holds 256 materials.
bool add_weathering(std::vector<VoxelMaterialDefinition>& materials, MaterialId target, WeatheringKind kind, float amount,
                    Float3 up = {0.0F, 1.0F, 0.0F}, std::uint32_t seed = 1U, std::string* error = nullptr);

} // namespace dve
