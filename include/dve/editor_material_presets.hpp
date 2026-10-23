#pragma once
#include "dve/editor_materials.hpp"
#include <array>
#include <set>
#include <span>
namespace dve::editor {
enum class MaterialPresetFeature : unsigned { Opaque, SolidTransmission, RoughTransmission, ClearCoat, NormalDetail, SurfaceDetail };
enum class MaterialPresetBackend : unsigned { CpuReference, CpuPlayer, NativePreview, Gpu };
enum class MaterialPresetSupport : unsigned { Supported, Approximate, Unavailable };
struct MaterialPresetCapability { MaterialPresetSupport support; std::string reason; };
enum class MaterialPresetProperty : unsigned {
    ColorR, ColorG, ColorB, Roughness, Metallic, Specular, Coverage, Transmission, Ior,
    AbsorptionR, AbsorptionG, AbsorptionB, AbsorptionDistance, Coat, CoatRoughness,
    DetailScale, DetailStrength, Pattern, DetailSpace, PatternScale,
    PatternRotationX, PatternRotationY, PatternRotationZ, GrainStretch, ColorVariation,
    RoughnessVariation, BumpHeight, PatternSeed, UvTileMeters, Count
};
struct MaterialPresetControl {
    MaterialPresetProperty property;
    std::string_view label, unit, help;
    float minimum, maximum, step;
    bool advanced{};
};
struct MaterialPresetAsset {
    unsigned schema{1}, revision{1};
    std::string id, name, family, tags, description;
    std::string provenance{"DVE original artistic defaults; not measured material fits"};
    std::string mappingUnits{"meters"};
    unsigned thumbnailRevision{1};
    VoxelMaterialDefinition defaults;
    std::vector<MaterialPresetFeature> requirements;
    std::vector<std::string> dependencies;
};
class MaterialPresetCatalog {
public:
    [[nodiscard]] static MaterialPresetCatalog starter();
    [[nodiscard]] bool add(MaterialPresetAsset, std::string* error = nullptr);
    [[nodiscard]] const MaterialPresetAsset* find(std::string_view id) const;
    [[nodiscard]] const std::vector<MaterialPresetAsset>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::vector<const MaterialPresetAsset*> search(std::string_view query, std::string_view family = {}) const;
private:
    std::vector<MaterialPresetAsset> entries_;
};
[[nodiscard]] MaterialPresetCapability material_preset_capability(const MaterialPresetAsset&, MaterialPresetBackend);
[[nodiscard]] std::string_view material_preset_support_label(MaterialPresetSupport);
[[nodiscard]] std::span<const MaterialPresetControl> material_preset_controls();
[[nodiscard]] float material_preset_value(const VoxelMaterialDefinition&, MaterialPresetProperty);
[[nodiscard]] bool set_material_preset_value(VoxelMaterialDefinition&, MaterialPresetProperty, float, std::string* error = nullptr);
// Preserve physical gameplay properties; a preset replaces the complete visual definition.
[[nodiscard]] VoxelMaterialDefinition instantiate_material_preset(const MaterialPresetAsset&, const VoxelMaterialDefinition& physical);
[[nodiscard]] bool reset_material_preset_property(VoxelMaterialDefinition&, const MaterialPresetCatalog&, MaterialPresetProperty, std::string* error = nullptr);
[[nodiscard]] bool update_material_preset(VoxelMaterialDefinition&, const MaterialPresetAsset&, bool preserveOverrides, std::string* error = nullptr);
[[nodiscard]] bool write_material_preset(const std::filesystem::path&, const MaterialPresetAsset&, std::string* error = nullptr);
[[nodiscard]] std::optional<MaterialPresetAsset> read_material_preset(const std::filesystem::path&, std::string* error = nullptr);
[[nodiscard]] bool validate_material_preset(const MaterialPresetAsset&, std::string* error = nullptr);
}
