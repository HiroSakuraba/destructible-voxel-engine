#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "dve/material_layers.hpp"

namespace dve {

enum class MaterialDecalSemantic : std::uint8_t {
    BulletImpact,
    Scorch,
    Crack,
    Dirt,
    Blood,
    Paint,
    StructuralStress,
    Wetness,
    FluidResidue,
};

enum class MaterialDecalChannel : std::uint32_t {
    NoChannels = 0U,
    BaseColor = 1U << 0U,
    Normal = 1U << 1U,
    Roughness = 1U << 2U,
    Metallic = 1U << 3U,
    Emissive = 1U << 4U,
    Opacity = 1U << 5U,
    Height = 1U << 6U,
    All = (1U << 7U) - 1U,
};

[[nodiscard]] constexpr MaterialDecalChannel operator|(
    MaterialDecalChannel left, MaterialDecalChannel right) noexcept {
    return static_cast<MaterialDecalChannel>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr bool has_decal_channel(
    MaterialDecalChannel channels, MaterialDecalChannel channel) noexcept {
    return (static_cast<std::uint32_t>(channels) & static_cast<std::uint32_t>(channel)) != 0U;
}

// CPU/deferred authoring representation shared by polygon and voxel-derived surfaces. The
// tangent, bitangent, and projection normal form an orthonormal box projection frame.
struct MaterialDecal {
    std::uint64_t id{};
    MaterialDecalSemantic semantic{MaterialDecalSemantic::Dirt};
    Float3 center{};
    Float3 tangent{1.0F, 0.0F, 0.0F};
    Float3 bitangent{0.0F, 1.0F, 0.0F};
    Float3 projectionNormal{0.0F, 0.0F, 1.0F};
    Float3 halfExtents{0.5F, 0.5F, 0.1F};
    float edgeFeather{0.1F};
    float normalFadeStart{0.25F};
    std::int32_t priority{};
    MaterialDecalChannel channels{MaterialDecalChannel::All};
    MaterialLayerSample layer{};
    bool enabled{true};
};

struct MaterialDecalCompositeStats {
    std::uint32_t candidates{};
    std::uint32_t projected{};
    std::uint32_t applied{};
    float maximumCoverage{};
};

[[nodiscard]] const char* material_decal_semantic_label(MaterialDecalSemantic semantic) noexcept;
[[nodiscard]] bool validate_material_decal(const MaterialDecal& decal, std::string* error = nullptr) noexcept;
[[nodiscard]] float material_decal_projection_coverage(
    const MaterialDecal& decal,
    Float3 worldPosition,
    Float3 worldNormal) noexcept;

[[nodiscard]] MaterialSurfaceSample composite_material_decals(
    MaterialSurfaceSample base,
    Float3 worldPosition,
    Float3 worldNormal,
    std::span<const MaterialDecal> decals,
    MaterialDecalCompositeStats* stats = nullptr);

// Deterministic CPU clustered-decal index. It is an authoring/reference structure rather than a
// production GPU cluster builder, but it exercises the same bounded candidate-query contract.
class MaterialDecalGrid {
public:
    explicit MaterialDecalGrid(float cellSizeMeters = 2.0F);

    [[nodiscard]] bool rebuild(std::span<const MaterialDecal> decals, std::string* error = nullptr);
    [[nodiscard]] std::vector<std::uint32_t> query(Float3 worldPosition) const;
    [[nodiscard]] std::span<const MaterialDecal> decals() const noexcept { return decals_; }
    [[nodiscard]] float cell_size() const noexcept { return cellSizeMeters_; }

private:
    struct CellKey {
        std::int32_t x{};
        std::int32_t y{};
        std::int32_t z{};
        friend bool operator==(const CellKey&, const CellKey&) = default;
    };
    struct CellHash {
        std::size_t operator()(const CellKey& key) const noexcept;
    };

    [[nodiscard]] CellKey cell_for(Float3 position) const noexcept;

    float cellSizeMeters_{2.0F};
    std::vector<MaterialDecal> decals_;
    std::unordered_map<CellKey, std::vector<std::uint32_t>, CellHash> cells_;
};

} // namespace dve
