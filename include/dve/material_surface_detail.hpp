#pragma once
#include <array>
#include <cstdint>
#include "dve/types.hpp"
namespace dve {
// Original, self-contained procedural maps. Distances are metres; UV modes explicitly
// declare the metric size of one UV tile. No external texture/provenance dependency.
// Marble, Strata and Cellular are solid (3D) textures: carving or cutting the object reveals the
// same pattern continuing inside. Per-pattern meaning of grainStretch:
//   Wood, Paper - grain stretch along local x (periods);  Leather, Mineral, Paint - unused;
//   Marble      - vein sharpness: veins are (1 - |sin|)^(grainStretch / 4) (thin when large);
//   Strata      - how far layers run before they undulate, in periods / 4;
//   Cellular    - crack width between stones: 1.6 / grainStretch of a cell.
// Marble veins are planes of constant local x, strata are layers of constant local y (both turned
// by rotationRadians); Cellular stones are jittered cells of size periodMeters.
enum class SurfacePattern : std::uint8_t { NoPattern, Wood, Leather, Paper, Mineral, Paint, Marble, Strata, Cellular };
inline constexpr SurfacePattern kLastSurfacePattern = SurfacePattern::Cellular;
// Patterns added after DVOX 1.11 / polygon asset 1.8 / material library v11 (files using them are
// written at the next version).
[[nodiscard]] inline bool surface_pattern_is_solid_texture(SurfacePattern p) noexcept {
    return p == SurfacePattern::Marble || p == SurfacePattern::Strata || p == SurfacePattern::Cellular;
}
enum class SurfaceDetailSpace : std::uint8_t { Object, World, UV0, UV1 };
struct MaterialSurfaceDetail {
    SurfacePattern pattern{SurfacePattern::NoPattern};
    SurfaceDetailSpace space{SurfaceDetailSpace::Object};
    std::uint32_t seed{1};
    float periodMeters{.02F};
    Float3 rotationRadians{};
    float grainStretch{20};
    float colorVariation{.25F};
    float roughnessVariation{.15F};
    float bumpHeightMeters{.00002F};
    float uvTileMeters{1};
};
inline constexpr std::size_t kSurfaceDetailFloatCount=9;
[[nodiscard]] std::array<float,kSurfaceDetailFloatCount> surface_detail_values(const MaterialSurfaceDetail&) noexcept;
void set_surface_detail_values(MaterialSurfaceDetail&,const std::array<float,kSurfaceDetailFloatCount>&) noexcept;
[[nodiscard]] bool material_surface_detail_valid(const MaterialSurfaceDetail&) noexcept;
[[nodiscard]] std::uint64_t material_surface_detail_hash(const MaterialSurfaceDetail&) noexcept;
struct MaterialSurfaceContext {
    Float3 objectPositionMeters{},worldPositionMeters{};
    Float2 uv0{},uv1{};
    bool hasUv{};
    RigidTransform pose{}; // rotation transports object-space normal perturbations
    Float3 tangent{1,0,0},bitangent{0,1,0}; // world-space UV frame
    float footprintMeters{};
};
struct SurfacePatternSample { float value{}; Float3 gradientPerMeter{}; };
[[nodiscard]] SurfacePatternSample sample_surface_pattern(const MaterialSurfaceDetail&,Float3 positionMeters,float footprintMeters=0) noexcept;
// The pattern as a height in [0, 1] (0.5 + 0.5 x value): a procedural height source for voxel
// displacement and parallax when a material has no height map (dve/voxel_displacement.hpp). Object
// and World spaces only; footprintMeters filters as sample_surface_pattern does.
[[nodiscard]] bool surface_pattern_is_height_source(const MaterialSurfaceDetail&) noexcept;
[[nodiscard]] float surface_pattern_height(const MaterialSurfaceDetail&, Float3 positionMeters, float footprintMeters = 0) noexcept;
struct VoxelMaterialDefinition;
// Resolves color, perceptual roughness and bounded tangent-plane bump at one hit.
// Authored settings stay intact in the source material; output values are shading-only.
void apply_material_surface_detail(VoxelMaterialDefinition&,Float3& worldNormal,const MaterialSurfaceContext&) noexcept;
}
