#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dve/polygon_asset.hpp"

namespace dve {

enum class MaterialDiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error,
};

enum class MaterialAuthoringFeature : std::uint8_t {
    StandardPbr,
    Unlit,
    EmissiveShading,
    Subsurface,
    TwoSidedFoliage,
    ClearCoat,
    OpaqueBlend,
    MaskedBlend,
    TranslucentBlend,
    BaseColorTexture,
    MetallicRoughnessTexture,
    NormalTexture,
    EmissiveTexture,
    OpacityTexture,
    Uv0Mapping,
    Uv1Mapping,
    WorldTriplanar,
    ObjectTriplanar,
    DetailColor,
    DetailNormal,
    DetailRoughness,
    OffsetParallax,
    SteepParallax,
    ParallaxOcclusion,
    UniformMaterialLayers,
    PerPixelMaterialLayers,
    TextureGenerationCheckedResidency,
    VertexDisplacement,
    ProjectedDecals,
    DestructionInteriorMaterials,
};

struct MaterialFeatureCapability {
    MaterialAuthoringFeature feature{MaterialAuthoringFeature::StandardPbr};
    bool authored{};
    bool cpuReference{};
    bool livePolygonMain{};
    bool shadowCaster{};
    bool visualOnly{};
    bool geometryAffecting{};
    bool collisionAffecting{};
    std::string note;
};

struct MaterialCostEstimate {
    std::uint32_t minimumTextureSamplesPerFragment{};
    std::uint32_t maximumTextureSamplesPerFragment{};
    std::uint32_t maximumHeightSamplesPerFragment{};
    std::uint32_t triplanarMultiplier{1U};
    bool requiresTangents{};
    bool requiresUv1{};
    bool requiresTransparentSorting{};
    std::string tier{"none"};
};

struct MaterialAuthoringDiagnostic {
    MaterialDiagnosticSeverity severity{MaterialDiagnosticSeverity::Info};
    std::string code;
    std::uint32_t materialIndex{};
    std::string message;
};

struct PolygonMaterialAuthoringEntry {
    std::uint32_t materialIndex{};
    std::string name;
    bool livePolygonParity{};
    MaterialCostEstimate cost;
    std::vector<MaterialFeatureCapability> features;
    std::vector<MaterialAuthoringDiagnostic> diagnostics;
};

struct PolygonMaterialAuthoringReport {
    bool assetValid{};
    std::uint64_t objectId{};
    std::uint64_t contentHash{};
    std::uint32_t materialCount{};
    std::uint32_t errorCount{};
    std::uint32_t warningCount{};
    std::uint32_t infoCount{};
    std::vector<PolygonMaterialAuthoringEntry> materials;
    std::vector<MaterialAuthoringDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept { return errorCount != 0U; }
    [[nodiscard]] bool has_warnings() const noexcept { return warningCount != 0U; }
};

[[nodiscard]] std::string_view material_feature_label(MaterialAuthoringFeature feature) noexcept;
[[nodiscard]] std::string_view material_diagnostic_severity_label(MaterialDiagnosticSeverity severity) noexcept;

// Global capability rows consumed by the native material inspector. These rows make support
// boundaries explicit even when a feature (for example decals) is scene-authored rather than
// embedded in one polygon material.
[[nodiscard]] std::vector<MaterialFeatureCapability> material_editor_capabilities();

// CPU-only material authoring analysis. This does not create an RHI device, compile shaders,
// allocate GPU resources, or execute a rendering backend. The live-path fields describe the
// currently implemented polygon/shadow behavior so the editor can distinguish authored data
// from functionality that is only executable in the reference renderer.
[[nodiscard]] PolygonMaterialAuthoringReport analyze_polygon_material_authoring(
    const CookedPolygonAsset& asset);

[[nodiscard]] std::string format_material_authoring_report_text(
    const PolygonMaterialAuthoringReport& report);
[[nodiscard]] std::string format_material_authoring_report_json(
    const PolygonMaterialAuthoringReport& report);

[[nodiscard]] bool write_material_authoring_report(
    const std::filesystem::path& path,
    const PolygonMaterialAuthoringReport& report,
    bool json,
    std::string* error = nullptr);

} // namespace dve
