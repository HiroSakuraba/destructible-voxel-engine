#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/material_parameter_collection.hpp"

namespace dve {

using MasterMaterialId = std::uint32_t;
inline constexpr MasterMaterialId kInvalidMasterMaterialId = 0;
using MaterialInstanceId = std::uint32_t;
inline constexpr MaterialInstanceId kInvalidMaterialInstanceId = 0;

// Reserved names mapped onto VoxelMaterialDefinition fields.
inline constexpr std::string_view kBaseColorParam = "BaseColor";
inline constexpr std::string_view kEmissiveParam = "Emissive";
inline constexpr std::string_view kMetallicParam = "Metallic";
inline constexpr std::string_view kRoughnessParam = "Roughness";
inline constexpr std::string_view kSpecularParam = "Specular";
inline constexpr std::string_view kTransparentParam = "Transparent";
inline constexpr std::string_view kSubsurfaceColorParam = "SubsurfaceColor";
inline constexpr std::string_view kSubsurfaceScatterDistanceParam = "SubsurfaceScatterDistanceMeters";
inline constexpr std::string_view kClearCoatParam = "ClearCoat";
inline constexpr std::string_view kClearCoatRoughnessParam = "ClearCoatRoughness";
inline constexpr std::string_view kFoliageColorParam = "FoliageColor";
inline constexpr std::string_view kFoliageTransmittanceParam = "FoliageTransmittance";
inline constexpr std::string_view kFoliageWrapParam = "FoliageWrap";
inline constexpr std::string_view kStandardSurfaceMasterName = "Engine/Materials/StandardSurface";
inline constexpr float kStandardSurfaceRoughness = 0.62F;
// Specular follows the engine's Unreal-style convention: 0.25 produces about 2% dielectric F0.
inline constexpr float kStandardSurfaceSpecular = 0.25F;

enum class MaterialGlobalCombine : std::uint8_t {
    Replace,
    Add,
    Multiply,
};

struct MaterialScalarParam {
    std::string name;
    float defaultValue{0.0F};
    float minValue{0.0F};
    float maxValue{1.0F};
};

struct MaterialVectorParam {
    std::string name;
    Float4 defaultValue{1.0F, 1.0F, 1.0F, 1.0F};
};

struct MaterialSwitchParam {
    std::string name;
    bool defaultValue{false};
};

struct MaterialParameterSchema {
    std::vector<MaterialScalarParam> scalars;
    std::vector<MaterialVectorParam> vectors;
    std::vector<MaterialSwitchParam> switches;

    [[nodiscard]] const MaterialScalarParam* find_scalar(std::string_view name) const noexcept;
    [[nodiscard]] const MaterialVectorParam* find_vector(std::string_view name) const noexcept;
    [[nodiscard]] const MaterialSwitchParam* find_switch(std::string_view name) const noexcept;
};

// An explicit material-to-collection bridge. Unreal materials only see an MPC value when the
// material contains a corresponding collection node; the same rule applies here. A binding
// names the local schema parameter, the global collection parameter, and the combination mode.
struct MaterialGlobalScalarBinding {
    std::string parameterName;
    std::string globalName;
    MaterialGlobalCombine combine{MaterialGlobalCombine::Replace};
};

struct MaterialGlobalVectorBinding {
    std::string parameterName;
    std::string globalName;
    MaterialGlobalCombine combine{MaterialGlobalCombine::Multiply};
};

struct MasterMaterial {
    MasterMaterialId id{kInvalidMasterMaterialId};
    std::string name;
    MaterialShadingModel shadingModel{MaterialShadingModel::StandardPBR};
    MaterialBlendMode blendMode{MaterialBlendMode::Opaque};
    MaterialParameterSchema parameters;
    std::vector<MaterialGlobalScalarBinding> globalScalars;
    std::vector<MaterialGlobalVectorBinding> globalVectors;
};

// Canonical general-purpose object shader schema used by new projects. The returned master is
// unregistered (id 0); MaterialLibrary::add_master assigns its stable runtime id.
[[nodiscard]] MasterMaterial make_standard_surface_master_material();

struct MaterialInstance {
    MaterialInstanceId id{kInvalidMaterialInstanceId};
    std::string name;
    MaterialId materialId{};
    MasterMaterialId master{kInvalidMasterMaterialId};
    std::optional<MaterialInstanceId> parent;

    std::unordered_map<std::string, float> scalarOverrides;
    std::unordered_map<std::string, Float4> vectorOverrides;
    std::unordered_map<std::string, bool> switchOverrides;
    std::vector<VoxelMaterialLayer> layers;

    float densityKilogramsPerCubicMeter{1000.0F};
    float structuralStrength{1.0F};
    float fractureResistance{1.0F};
    float flammability{0.0F};
    float thermalConductivity{0.0F};
    bool structural{true};
};

enum class MaterialResolveErrorCode : std::uint8_t {
    NoError,
    UnknownInstance,
    UnknownMaster,
    UnknownParent,
    CyclicInheritance,
    InconsistentMaster,
    UnknownParameter,
    ScalarOutOfRange,
    InvalidGlobalBinding,
    GlobalParameterOutOfRange,
    TooManyLayers,
    InvalidLayerWeight,
    UnknownLayerMaterial,
    CyclicLayerStack,
};

struct MaterialResolveResult {
    VoxelMaterialDefinition definition;
    std::unordered_map<std::string, float> resolvedScalars;
    std::unordered_map<std::string, Float4> resolvedVectors;
    std::unordered_map<std::string, bool> resolvedSwitches;
    MasterMaterialId master{kInvalidMasterMaterialId};
    MaterialResolveErrorCode error{MaterialResolveErrorCode::NoError};
    std::string errorDetail;
    [[nodiscard]] bool success() const noexcept { return error == MaterialResolveErrorCode::NoError; }
};

[[nodiscard]] MaterialResolveResult resolve_material_instance(
    MaterialInstanceId id,
    const std::unordered_map<MasterMaterialId, MasterMaterial>& masters,
    const std::unordered_map<MaterialInstanceId, MaterialInstance>& instances,
    const MaterialParameterCollection* globals = nullptr);

// Blends only render-facing fields. Physics/gameplay fields and the base material's shading and
// blend model remain authoritative. This makes rust-over-metal and paint-over-concrete useful
// without accidentally changing density or turning an opaque voxel translucent.
[[nodiscard]] VoxelMaterialDefinition blend_material_layer(
    const VoxelMaterialDefinition& base,
    const VoxelMaterialDefinition& layer,
    float weight,
    MaterialLayerBlendMode blendMode) noexcept;

// Resolves layer stacks embedded in a material vector (where MaterialId is the vector index),
// useful for cooked DVOX/editor assets that are not backed by a MaterialLibrary.
[[nodiscard]] std::optional<std::vector<VoxelMaterialDefinition>> resolve_voxel_material_layers(
    const std::vector<VoxelMaterialDefinition>& materials,
    std::string* error = nullptr);

class MaterialLibrary {
public:
    [[nodiscard]] MasterMaterialId add_master(MasterMaterial master, std::string* error = nullptr);
    [[nodiscard]] bool update_master(const MasterMaterial& master, std::string* error = nullptr);
    [[nodiscard]] const MasterMaterial* find_master(MasterMaterialId id) const noexcept;
    [[nodiscard]] std::optional<MasterMaterialId> find_master_by_name(std::string_view name) const;

    [[nodiscard]] MaterialInstanceId add_instance(MaterialInstance instance, std::string* error = nullptr);
    [[nodiscard]] bool update_instance(const MaterialInstance& instance, std::string* error = nullptr);
    [[nodiscard]] const MaterialInstance* find_instance(MaterialInstanceId id) const noexcept;
    [[nodiscard]] std::optional<MaterialInstanceId> find_instance_by_material_id(MaterialId materialId) const;

    [[nodiscard]] bool resolve_all(std::string* error = nullptr);
    [[nodiscard]] const MaterialResolveResult* last_result(MaterialInstanceId id) const;
    [[nodiscard]] const VoxelMaterialDefinition* resolved(MaterialId materialId) const noexcept;

    [[nodiscard]] bool set_runtime_scalar(MaterialId materialId, std::string_view name, float value, std::string* error = nullptr);
    [[nodiscard]] bool set_runtime_vector(MaterialId materialId, std::string_view name, Float4 value, std::string* error = nullptr);
    [[nodiscard]] std::optional<float> runtime_scalar(MaterialId materialId, std::string_view name) const;
    [[nodiscard]] std::optional<Float4> runtime_vector(MaterialId materialId, std::string_view name) const;
    bool clear_runtime_overrides(MaterialId materialId);

    [[nodiscard]] bool set_runtime_layer_weight(MaterialId materialId, std::size_t layerIndex, float weight,
                                                std::string* error = nullptr);
    [[nodiscard]] std::optional<float> runtime_layer_weight(MaterialId materialId, std::size_t layerIndex) const;
    bool clear_runtime_layer_weights(MaterialId materialId);
    [[nodiscard]] const std::vector<VoxelMaterialLayer>* layers(MaterialId materialId) const noexcept;

    // The bridge used by world.set_global. Updates are validate-then-commit: a value that would
    // push any explicitly-bound material parameter outside its schema range is rejected and the
    // previous global/material state remains intact.
    [[nodiscard]] bool set_global_scalar(std::string_view name, float value, std::string* error = nullptr);
    [[nodiscard]] bool set_global_vector(std::string_view name, Float4 value, std::string* error = nullptr);
    [[nodiscard]] bool uses_global_scalar(std::string_view name) const noexcept;
    [[nodiscard]] bool uses_global_vector(std::string_view name) const noexcept;
    [[nodiscard]] bool save_parameter_collection(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] bool load_parameter_collection(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] const MaterialParameterCollection& parameter_collection() const noexcept { return globals_; }

private:
    [[nodiscard]] bool validate_master(const MasterMaterial& master, std::string* error) const;
    void apply_runtime_overrides(MaterialId materialId, VoxelMaterialDefinition& definition) const;

    std::unordered_map<MasterMaterialId, MasterMaterial> masters_;
    std::unordered_map<MaterialInstanceId, MaterialInstance> instances_;
    std::unordered_map<MaterialInstanceId, MaterialResolveResult> results_;
    std::unordered_map<MaterialId, MaterialInstanceId> materialIdToInstance_;
    std::unordered_map<MaterialId, VoxelMaterialDefinition> resolvedByMaterialId_;
    std::unordered_map<MaterialId, std::unordered_map<std::string, float>> runtimeScalars_;
    std::unordered_map<MaterialId, std::unordered_map<std::string, Float4>> runtimeVectors_;
    std::unordered_map<MaterialId, std::unordered_map<std::size_t, float>> runtimeLayerWeights_;
    MaterialParameterCollection globals_;
    MasterMaterialId nextMasterId_{1};
    MaterialInstanceId nextInstanceId_{1};
};

} // namespace dve
