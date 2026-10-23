#pragma once
#include <array>
#include <cstdint>
#include "dve/types.hpp"
namespace dve {
// Original, self-contained procedural maps. Distances are metres; UV modes explicitly
// declare the metric size of one UV tile. No external texture/provenance dependency.
enum class SurfacePattern : std::uint8_t { None, Wood, Leather, Paper, Mineral, Paint };
enum class SurfaceDetailSpace : std::uint8_t { Object, World, UV0, UV1 };
struct MaterialSurfaceDetail {
    SurfacePattern pattern{SurfacePattern::None};
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
struct VoxelMaterialDefinition;
// Resolves color, perceptual roughness and bounded tangent-plane bump at one hit.
// Authored settings stay intact in the source material; output values are shading-only.
void apply_material_surface_detail(VoxelMaterialDefinition&,Float3& worldNormal,const MaterialSurfaceContext&) noexcept;
}
