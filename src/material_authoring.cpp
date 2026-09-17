#include "dve/material_authoring.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dve {
namespace {

struct BindingDescriptor {
    MaterialAuthoringFeature feature;
    const PolygonTextureBinding* binding;
    std::string_view label;
    bool liveMain;
    bool shadow;
};

void add_diagnostic(PolygonMaterialAuthoringReport& report,
                    PolygonMaterialAuthoringEntry* entry,
                    MaterialDiagnosticSeverity severity,
                    std::string code,
                    std::uint32_t materialIndex,
                    std::string message) {
    MaterialAuthoringDiagnostic diagnostic{
        severity, std::move(code), materialIndex, std::move(message)};
    switch (severity) {
        case MaterialDiagnosticSeverity::Info: ++report.infoCount; break;
        case MaterialDiagnosticSeverity::Warning: ++report.warningCount; break;
        case MaterialDiagnosticSeverity::Error: ++report.errorCount; break;
    }
    report.diagnostics.push_back(diagnostic);
    if (entry != nullptr) entry->diagnostics.push_back(std::move(diagnostic));
}

void add_feature(PolygonMaterialAuthoringEntry& entry,
                 MaterialAuthoringFeature feature,
                 bool liveMain,
                 bool shadow,
                 bool visualOnly,
                 std::string note = {}) {
    entry.features.push_back(MaterialFeatureCapability{
        feature, true, true, liveMain, shadow, visualOnly, false, false, std::move(note)});
}

bool uses_uv1(const PolygonMaterialBinding& binding) noexcept {
    if (binding.mapping.mappingMode == MaterialMappingMode::UV1) return true;
    const std::array<const PolygonTextureBinding*, 9> bindings{{
        &binding.baseColor, &binding.metallicRoughness, &binding.normal, &binding.emissive,
        &binding.opacity, &binding.height, &binding.detailBaseColor, &binding.detailNormal,
        &binding.detailRoughness}};
    if (std::any_of(bindings.begin(), bindings.end(), [](const PolygonTextureBinding* value) {
        return value->texture.has_value() && value->texcoord == 1U;
    })) return true;
    return std::any_of(binding.layers.begin(), binding.layers.end(), [](const PolygonMaterialLayerBinding& layer) {
        return (layer.mask.texture && layer.mask.texcoord == 1U) ||
               (layer.height.texture && layer.height.texcoord == 1U);
    });
}

bool has_non_degenerate_uv1(const CookedPolygonAsset& asset) noexcept {
    if (asset.vertices.empty()) return false;
    const Float2 first = asset.vertices.front().texcoord1;
    return std::any_of(asset.vertices.begin() + 1, asset.vertices.end(), [&](const PolygonVertex& vertex) {
        return std::abs(vertex.texcoord1.x - first.x) > 1.0e-6F ||
               std::abs(vertex.texcoord1.y - first.y) > 1.0e-6F;
    });
}

bool has_usable_tangents(const CookedPolygonAsset& asset) noexcept {
    return std::all_of(asset.vertices.begin(), asset.vertices.end(), [](const PolygonVertex& vertex) {
        const float lengthSquared = vertex.tangent.x * vertex.tangent.x +
                                    vertex.tangent.y * vertex.tangent.y +
                                    vertex.tangent.z * vertex.tangent.z;
        return std::isfinite(lengthSquared) && lengthSquared > 1.0e-8F &&
               std::isfinite(vertex.tangent.w) && std::abs(vertex.tangent.w) > 0.5F;
    });
}

std::uint32_t height_sample_maximum(const MaterialMappingSettings& mapping) noexcept {
    switch (mapping.heightMode) {
        case HeightMappingMode::Off: return 0U;
        case HeightMappingMode::OffsetParallax: return 1U;
        case HeightMappingMode::SteepParallax: return mapping.maximumHeightSteps;
        case HeightMappingMode::ParallaxOcclusion:
            return mapping.maximumHeightSteps + mapping.refinementSteps;
    }
    return 0U;
}

std::string cost_tier(std::uint32_t samples) {
    if (samples == 0U) return "none";
    if (samples <= 3U) return "low";
    if (samples <= 8U) return "moderate";
    if (samples <= 20U) return "high";
    return "very-high";
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(character) << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
                break;
        }
    }
    return output.str();
}

bool is_data_texture(MaterialAuthoringFeature feature) noexcept {
    return feature == MaterialAuthoringFeature::MetallicRoughnessTexture ||
           feature == MaterialAuthoringFeature::NormalTexture ||
           feature == MaterialAuthoringFeature::OpacityTexture ||
           feature == MaterialAuthoringFeature::DetailNormal ||
           feature == MaterialAuthoringFeature::DetailRoughness;
}

} // namespace

std::string_view material_feature_label(MaterialAuthoringFeature feature) noexcept {
    switch (feature) {
        case MaterialAuthoringFeature::StandardPbr: return "Standard PBR";
        case MaterialAuthoringFeature::Unlit: return "Unlit";
        case MaterialAuthoringFeature::EmissiveShading: return "Emissive shading";
        case MaterialAuthoringFeature::Subsurface: return "Subsurface";
        case MaterialAuthoringFeature::TwoSidedFoliage: return "Two-sided foliage";
        case MaterialAuthoringFeature::ClearCoat: return "Clear coat";
        case MaterialAuthoringFeature::OpaqueBlend: return "Opaque blend";
        case MaterialAuthoringFeature::MaskedBlend: return "Masked blend";
        case MaterialAuthoringFeature::TranslucentBlend: return "Translucent blend";
        case MaterialAuthoringFeature::BaseColorTexture: return "Base-color texture";
        case MaterialAuthoringFeature::MetallicRoughnessTexture: return "Metallic/roughness texture";
        case MaterialAuthoringFeature::NormalTexture: return "Normal texture";
        case MaterialAuthoringFeature::EmissiveTexture: return "Emissive texture";
        case MaterialAuthoringFeature::OpacityTexture: return "Opacity texture";
        case MaterialAuthoringFeature::Uv0Mapping: return "UV0 mapping";
        case MaterialAuthoringFeature::Uv1Mapping: return "UV1 mapping";
        case MaterialAuthoringFeature::WorldTriplanar: return "World triplanar";
        case MaterialAuthoringFeature::ObjectTriplanar: return "Object triplanar";
        case MaterialAuthoringFeature::DetailColor: return "Detail color";
        case MaterialAuthoringFeature::DetailNormal: return "Detail normal";
        case MaterialAuthoringFeature::DetailRoughness: return "Detail roughness";
        case MaterialAuthoringFeature::OffsetParallax: return "Offset parallax";
        case MaterialAuthoringFeature::SteepParallax: return "Steep parallax";
        case MaterialAuthoringFeature::ParallaxOcclusion: return "Parallax occlusion";
        case MaterialAuthoringFeature::UniformMaterialLayers: return "Uniform material layers";
        case MaterialAuthoringFeature::PerPixelMaterialLayers: return "Per-pixel material layers";
        case MaterialAuthoringFeature::TextureGenerationCheckedResidency: return "Generation-checked texture residency";
        case MaterialAuthoringFeature::VertexDisplacement: return "Vertex displacement";
        case MaterialAuthoringFeature::ProjectedDecals: return "Projected decals";
        case MaterialAuthoringFeature::DestructionInteriorMaterials: return "Destruction interior materials";
    }
    return "Unknown";
}

std::vector<MaterialFeatureCapability> material_editor_capabilities() {
    return {
        {MaterialAuthoringFeature::PerPixelMaterialLayers,true,true,false,false,true,false,false,
         "CPU reference compositor supports paint, rust, dirt, wetness, snow, scorch, and fracture masks; live GPU parity is pending."},
        {MaterialAuthoringFeature::TextureGenerationCheckedResidency,true,true,false,false,false,false,false,
         "TextureResidencyManager provides mip generation, bounded uploads, LRU eviction, hot reload, and generation-checked references; pass-wide migration remains pending."},
        {MaterialAuthoringFeature::VertexDisplacement,true,true,false,false,false,true,true,
         "Deterministic offline subdivision and CPU normal-direction displacement support visual-only or collision-affecting polygon outputs; voxel surfaces remain visual-only without revoxelization."},
        {MaterialAuthoringFeature::ProjectedDecals,true,true,false,false,true,false,false,
         "Geometry-agnostic projected decals and a deterministic clustered CPU index support polygon and voxel-derived surface evaluators."},
        {MaterialAuthoringFeature::DestructionInteriorMaterials,true,true,false,false,false,false,false,
         "Newly exposed voxel faces can select interior, fracture, and optional fracture-overlay materials instead of inheriting the exterior finish."},
    };
}

std::string_view material_diagnostic_severity_label(MaterialDiagnosticSeverity severity) noexcept {
    switch (severity) {
        case MaterialDiagnosticSeverity::Info: return "info";
        case MaterialDiagnosticSeverity::Warning: return "warning";
        case MaterialDiagnosticSeverity::Error: return "error";
    }
    return "unknown";
}

PolygonMaterialAuthoringReport analyze_polygon_material_authoring(const CookedPolygonAsset& asset) {
    PolygonMaterialAuthoringReport report;
    report.objectId = asset.objectId;
    report.contentHash = asset.contentHash;
    report.materialCount = static_cast<std::uint32_t>(asset.materials.size());

    const PolygonAssetValidationResult validation = validate_polygon_asset(asset);
    report.assetValid = static_cast<bool>(validation);
    if (!validation) {
        add_diagnostic(report, nullptr, MaterialDiagnosticSeverity::Error,
                       "ASSET_INVALID", 0U, validation.message);
        return report;
    }

    const bool uv1Usable = has_non_degenerate_uv1(asset);
    const bool tangentsUsable = has_usable_tangents(asset);

    report.materials.reserve(asset.materials.size());
    for (std::uint32_t index = 0U; index < asset.materials.size(); ++index) {
        const VoxelMaterialDefinition& material = asset.materials[index];
        const PolygonMaterialBinding& binding = asset.materialBindings[index];
        PolygonMaterialAuthoringEntry entry;
        entry.materialIndex = index;
        entry.name = material.name;

        switch (material.shadingModel) {
            case MaterialShadingModel::StandardPBR:
                add_feature(entry, MaterialAuthoringFeature::StandardPbr, true, true, false,
                            "Complete CPU reference and current live polygon baseline.");
                break;
            case MaterialShadingModel::Unlit:
                add_feature(entry, MaterialAuthoringFeature::Unlit, false, true, false,
                            "CPU reference is executable; live polygon shading-model parity is pending.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_SHADING_MODEL_REFERENCE_ONLY", index,
                               "Unlit is currently reference-renderer-only in the polygon material path.");
                break;
            case MaterialShadingModel::Emissive:
                add_feature(entry, MaterialAuthoringFeature::EmissiveShading, false, true, false,
                            "Emissive textures are live, but the dedicated emissive shading model is pending.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_SHADING_MODEL_REFERENCE_ONLY", index,
                               "The dedicated emissive shading model is not yet implemented by the live polygon evaluator.");
                break;
            case MaterialShadingModel::Subsurface:
                add_feature(entry, MaterialAuthoringFeature::Subsurface, false, false, false,
                            "CPU approximation only; live transmission and thickness evaluation are pending.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_SHADING_MODEL_REFERENCE_ONLY", index,
                               "Subsurface is currently available only in the CPU reference renderer.");
                break;
            case MaterialShadingModel::TwoSidedFoliage:
                add_feature(entry, MaterialAuthoringFeature::TwoSidedFoliage, false, false, false,
                            "CPU transmission approximation only; live foliage parity is pending.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_SHADING_MODEL_REFERENCE_ONLY", index,
                               "Two-sided foliage is currently available only in the CPU reference renderer.");
                break;
            case MaterialShadingModel::ClearCoat:
                add_feature(entry, MaterialAuthoringFeature::ClearCoat, false, false, false,
                            "CPU second-lobe approximation only; live polygon coat IBL is pending.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_SHADING_MODEL_REFERENCE_ONLY", index,
                               "Clear coat is currently available only in the CPU reference renderer for polygons.");
                break;
        }

        switch (material.blendMode) {
            case MaterialBlendMode::Opaque:
                add_feature(entry, MaterialAuthoringFeature::OpaqueBlend, true, true, false);
                break;
            case MaterialBlendMode::Masked:
                add_feature(entry, MaterialAuthoringFeature::MaskedBlend, true, true, false,
                            "Alpha cutoff affects both main and shadow reference contracts.");
                if (!binding.baseColor.texture && !binding.opacity.texture &&
                    material.baseColor.w >= binding.alphaCutoff) {
                    add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                                   "MASK_HAS_NO_ALPHA_VARIATION", index,
                                   "Masked material has no texture-driven alpha and will behave as fully opaque.");
                }
                break;
            case MaterialBlendMode::Translucent:
                add_feature(entry, MaterialAuthoringFeature::TranslucentBlend, false, false, false,
                            "CPU sorted alpha blending is executable; production live translucency is pending.");
                entry.cost.requiresTransparentSorting = true;
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_TRANSLUCENCY_UNAVAILABLE", index,
                               "Translucency is reference-renderer-only and should not be treated as production live output.");
                break;
        }

        switch (binding.mapping.mappingMode) {
            case MaterialMappingMode::UV0:
                add_feature(entry, MaterialAuthoringFeature::Uv0Mapping, true, true, false);
                break;
            case MaterialMappingMode::UV1:
                add_feature(entry, MaterialAuthoringFeature::Uv1Mapping, false, false, false,
                            "CPU reference is executable; live UV1 selection is the next integration item.");
                entry.cost.requiresUv1 = true;
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_UV1_REFERENCE_ONLY", index,
                               "UV1 mapping is currently available only in the CPU reference renderer.");
                break;
            case MaterialMappingMode::WorldTriplanar:
                add_feature(entry, MaterialAuthoringFeature::WorldTriplanar, false, false, false,
                            "CPU reference uses three projections per mapped texture.");
                entry.cost.triplanarMultiplier = 3U;
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_TRIPLANAR_REFERENCE_ONLY", index,
                               "World triplanar mapping is currently available only in the CPU reference renderer.");
                break;
            case MaterialMappingMode::ObjectTriplanar:
                add_feature(entry, MaterialAuthoringFeature::ObjectTriplanar, false, false, false,
                            "CPU reference uses three object-space projections per mapped texture.");
                entry.cost.triplanarMultiplier = 3U;
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_TRIPLANAR_REFERENCE_ONLY", index,
                               "Object triplanar mapping is currently available only in the CPU reference renderer.");
                break;
        }

        const std::array<BindingDescriptor, 8> textureBindings{{
            {MaterialAuthoringFeature::BaseColorTexture, &binding.baseColor, "base color", true, true},
            {MaterialAuthoringFeature::MetallicRoughnessTexture, &binding.metallicRoughness,
             "metallic/roughness", true, false},
            {MaterialAuthoringFeature::NormalTexture, &binding.normal, "normal", true, false},
            {MaterialAuthoringFeature::EmissiveTexture, &binding.emissive, "emissive", true, false},
            {MaterialAuthoringFeature::OpacityTexture, &binding.opacity, "opacity", true, true},
            {MaterialAuthoringFeature::DetailColor, &binding.detailBaseColor, "detail color", false, false},
            {MaterialAuthoringFeature::DetailNormal, &binding.detailNormal, "detail normal", false, false},
            {MaterialAuthoringFeature::DetailRoughness, &binding.detailRoughness,
             "detail roughness", false, false},
        }};

        std::uint32_t coreTextureCount = 0U;
        std::uint32_t detailTextureCount = 0U;
        for (const BindingDescriptor& descriptor : textureBindings) {
            if (!descriptor.binding->texture) continue;
            const bool detail = descriptor.feature == MaterialAuthoringFeature::DetailColor ||
                                descriptor.feature == MaterialAuthoringFeature::DetailNormal ||
                                descriptor.feature == MaterialAuthoringFeature::DetailRoughness;
            add_feature(entry, descriptor.feature, descriptor.liveMain, descriptor.shadow, false,
                        descriptor.liveMain ? "Executable in the current live polygon evaluator."
                                            : "Executable in the CPU reference renderer; live evaluation is pending.");
            if (detail) ++detailTextureCount; else ++coreTextureCount;
            if (descriptor.feature == MaterialAuthoringFeature::NormalTexture ||
                descriptor.feature == MaterialAuthoringFeature::DetailNormal) {
                entry.cost.requiresTangents = binding.mapping.mappingMode == MaterialMappingMode::UV0 ||
                                              binding.mapping.mappingMode == MaterialMappingMode::UV1;
            }
            if (!descriptor.liveMain) {
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_DETAIL_REFERENCE_ONLY", index,
                               std::string(descriptor.label) +
                                   " is authored but currently evaluated only by the CPU reference renderer.");
            }
            if (is_data_texture(descriptor.feature) &&
                descriptor.binding->colorSpace == PolygonTextureColorSpace::Srgb) {
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "DATA_TEXTURE_MARKED_SRGB", index,
                               std::string(descriptor.label) +
                                   " is a data texture but is marked for sRGB decoding.");
            }
        }

        if (binding.detailBaseColor.texture && binding.mapping.detailColorStrength <= 1.0e-6F) {
            add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Info,
                           "DETAIL_COLOR_ZERO_STRENGTH", index,
                           "Detail-color texture is bound but its blend strength is zero.");
        }
        if (binding.detailNormal.texture && binding.mapping.detailNormalStrength <= 1.0e-6F) {
            add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Info,
                           "DETAIL_NORMAL_ZERO_STRENGTH", index,
                           "Detail-normal texture is bound but its blend strength is zero.");
        }
        if (binding.detailRoughness.texture && binding.mapping.detailRoughnessStrength <= 1.0e-6F) {
            add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Info,
                           "DETAIL_ROUGHNESS_ZERO_STRENGTH", index,
                           "Detail-roughness texture is bound but its blend strength is zero.");
        }

        if (uses_uv1(binding)) {
            entry.cost.requiresUv1 = true;
            if (!uv1Usable) {
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "UV1_DEGENERATE", index,
                               "Material requests UV1 but the asset's second UV set is constant.");
            }
        }
        if (entry.cost.requiresTangents && !tangentsUsable) {
            add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                           "TANGENT_BASIS_DEGENERATE", index,
                           "Normal mapping requires tangents, but one or more vertex tangents are degenerate.");
        }

        switch (binding.mapping.heightMode) {
            case HeightMappingMode::Off: break;
            case HeightMappingMode::OffsetParallax:
                add_feature(entry, MaterialAuthoringFeature::OffsetParallax, false, false, true,
                            "Visual-only UV displacement; geometry, collision, depth, and shadow silhouettes are unchanged.");
                break;
            case HeightMappingMode::SteepParallax:
                add_feature(entry, MaterialAuthoringFeature::SteepParallax, false, false, true,
                            "Visual-only UV displacement with angle-adaptive stepping.");
                break;
            case HeightMappingMode::ParallaxOcclusion:
                add_feature(entry, MaterialAuthoringFeature::ParallaxOcclusion, false, false, true,
                            "Visual-only UV displacement with binary refinement.");
                break;
        }
        entry.cost.maximumHeightSamplesPerFragment = height_sample_maximum(binding.mapping);
        if (binding.mapping.heightMode != HeightMappingMode::Off) {
            add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                           "PARALLAX_VISUAL_ONLY", index,
                           "Height mapping changes UVs only; it does not change geometry, collision, depth, or shadow silhouettes.");
            add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                           "LIVE_PARALLAX_REFERENCE_ONLY", index,
                           "Height mapping is currently executable only in the CPU reference renderer.");
        }

        std::uint32_t layerTextureSamples = 0U;
        if (!material.layers.empty()) {
            if (binding.layers.empty()) {
                add_feature(entry, MaterialAuthoringFeature::UniformMaterialLayers, true, true, false,
                            "Legacy layers use uniform weights in both CPU and current live scalar resolution.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Info,
                               "LAYERS_UNIFORM_ONLY", index,
                               "Material layers have no per-pixel mask bindings and therefore use uniform weights.");
            } else {
                add_feature(entry, MaterialAuthoringFeature::PerPixelMaterialLayers, false, false, true,
                            "CPU reference evaluates height-aware masks, RNM normals, perceptual roughness, bounded metallic, additive emissive, and independent opacity policies.");
                add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Warning,
                               "LIVE_LAYER_MASKS_REFERENCE_ONLY", index,
                               "Per-pixel material layers are executable in the CPU reference path but not yet in the live GPU or shadow paths.");
                if (binding.layers.size() < material.layers.size()) {
                    add_diagnostic(report, &entry, MaterialDiagnosticSeverity::Info,
                                   "LAYER_BINDING_COUNT_MISMATCH", index,
                                   "Some material layers have no per-pixel binding and retain their legacy uniform weights.");
                }
                const std::size_t count = std::min(material.layers.size(), binding.layers.size());
                for (std::size_t layerIndex = 0U; layerIndex < count; ++layerIndex) {
                    const VoxelMaterialLayer& authoredLayer = material.layers[layerIndex];
                    const PolygonMaterialLayerBinding& layerBinding = binding.layers[layerIndex];
                    if (!authoredLayer.enabled || !layerBinding.enabled || authoredLayer.weight <= 0.0F) continue;
                    layerTextureSamples += layerBinding.mask.texture ? 1U : 0U;
                    layerTextureSamples += layerBinding.height.texture ? 1U : 0U;
                    if (authoredLayer.sourceMaterial < asset.materialBindings.size()) {
                        const PolygonMaterialBinding& sourceBinding = asset.materialBindings[authoredLayer.sourceMaterial];
                        layerTextureSamples += sourceBinding.baseColor.texture ? 1U : 0U;
                        layerTextureSamples += sourceBinding.metallicRoughness.texture ? 1U : 0U;
                        layerTextureSamples += sourceBinding.normal.texture ? 1U : 0U;
                        layerTextureSamples += sourceBinding.emissive.texture ? 1U : 0U;
                        layerTextureSamples += sourceBinding.opacity.texture ? 1U : 0U;
                        layerTextureSamples += sourceBinding.height.texture ? 1U : 0U;
                    }
                }
            }
        }

        const std::uint32_t mappedMultiplier = entry.cost.triplanarMultiplier;
        entry.cost.minimumTextureSamplesPerFragment = coreTextureCount * mappedMultiplier;
        entry.cost.maximumTextureSamplesPerFragment =
            (coreTextureCount + detailTextureCount + layerTextureSamples) * mappedMultiplier +
            entry.cost.maximumHeightSamplesPerFragment;
        entry.cost.tier = cost_tier(entry.cost.maximumTextureSamplesPerFragment);

        entry.livePolygonParity = std::none_of(
            entry.features.begin(), entry.features.end(), [](const MaterialFeatureCapability& feature) {
                return feature.authored && !feature.livePolygonMain;
            });
        report.materials.push_back(std::move(entry));
    }

    return report;
}

std::string format_material_authoring_report_text(const PolygonMaterialAuthoringReport& report) {
    std::ostringstream output;
    output << "DVE material authoring report\n"
           << "object_id: " << report.objectId << '\n'
           << "content_hash: " << report.contentHash << '\n'
           << "asset_valid: " << (report.assetValid ? "yes" : "no") << '\n'
           << "materials: " << report.materialCount << '\n'
           << "diagnostics: " << report.errorCount << " error, "
           << report.warningCount << " warning, " << report.infoCount << " info\n";

    output << "editor_capabilities:\n";
    for (const MaterialFeatureCapability& feature : material_editor_capabilities()) {
        output << "  - " << material_feature_label(feature.feature)
               << ": cpu=" << (feature.cpuReference ? "yes" : "no")
               << ", live=" << (feature.livePolygonMain ? "yes" : "no")
               << ", shadow=" << (feature.shadowCaster ? "yes" : "no");
        if (feature.visualOnly) output << ", visual-only=yes";
        if (feature.geometryAffecting) output << ", geometry-affecting=yes";
        if (feature.collisionAffecting) output << ", collision-affecting=yes";
        if (!feature.note.empty()) output << " -- " << feature.note;
        output << '\n';
    }

    for (const PolygonMaterialAuthoringEntry& material : report.materials) {
        output << "\n[" << material.materialIndex << "] " << material.name << '\n'
               << "  live_polygon_parity: " << (material.livePolygonParity ? "yes" : "no") << '\n'
               << "  estimated_samples: " << material.cost.minimumTextureSamplesPerFragment
               << ".." << material.cost.maximumTextureSamplesPerFragment
               << " (height max " << material.cost.maximumHeightSamplesPerFragment
               << ", tier " << material.cost.tier << ")\n";
        output << "  features:\n";
        for (const MaterialFeatureCapability& feature : material.features) {
            output << "    - " << material_feature_label(feature.feature)
                   << ": cpu=" << (feature.cpuReference ? "yes" : "no")
                   << ", live=" << (feature.livePolygonMain ? "yes" : "no")
                   << ", shadow=" << (feature.shadowCaster ? "yes" : "no");
            if (feature.visualOnly) output << ", visual-only=yes";
            if (feature.geometryAffecting) output << ", geometry-affecting=yes";
            if (feature.collisionAffecting) output << ", collision-affecting=yes";
            if (!feature.note.empty()) output << " -- " << feature.note;
            output << '\n';
        }
        if (!material.diagnostics.empty()) {
            output << "  diagnostics:\n";
            for (const MaterialAuthoringDiagnostic& diagnostic : material.diagnostics) {
                output << "    - " << material_diagnostic_severity_label(diagnostic.severity)
                       << " " << diagnostic.code << ": " << diagnostic.message << '\n';
            }
        }
    }
    if (report.materials.empty() && !report.diagnostics.empty()) {
        output << "\ndiagnostics:\n";
        for (const MaterialAuthoringDiagnostic& diagnostic : report.diagnostics) {
            output << "  - " << material_diagnostic_severity_label(diagnostic.severity)
                   << " " << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    return output.str();
}

std::string format_material_authoring_report_json(const PolygonMaterialAuthoringReport& report) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schema\": \"dve.material-authoring-report.v1\",\n"
           << "  \"objectId\": " << report.objectId << ",\n"
           << "  \"contentHash\": " << report.contentHash << ",\n"
           << "  \"assetValid\": " << (report.assetValid ? "true" : "false") << ",\n"
           << "  \"summary\": {\"materials\": " << report.materialCount
           << ", \"errors\": " << report.errorCount
           << ", \"warnings\": " << report.warningCount
           << ", \"info\": " << report.infoCount << "},\n"
           << "  \"editorCapabilities\": [";
    const auto editorCapabilities = material_editor_capabilities();
    for (std::size_t capabilityIndex = 0; capabilityIndex < editorCapabilities.size(); ++capabilityIndex) {
        const MaterialFeatureCapability& feature = editorCapabilities[capabilityIndex];
        if (capabilityIndex != 0U) output << ',';
        output << "{\"name\": \"" << json_escape(material_feature_label(feature.feature))
               << "\", \"cpuReference\": " << (feature.cpuReference ? "true" : "false")
               << ", \"livePolygonMain\": " << (feature.livePolygonMain ? "true" : "false")
               << ", \"shadowCaster\": " << (feature.shadowCaster ? "true" : "false")
               << ", \"visualOnly\": " << (feature.visualOnly ? "true" : "false")
               << ", \"geometryAffecting\": " << (feature.geometryAffecting ? "true" : "false")
               << ", \"collisionAffecting\": " << (feature.collisionAffecting ? "true" : "false")
               << ", \"note\": \"" << json_escape(feature.note) << "\"}";
    }
    output << "],\n"
           << "  \"materials\": [\n";
    for (std::size_t materialIndex = 0; materialIndex < report.materials.size(); ++materialIndex) {
        const PolygonMaterialAuthoringEntry& material = report.materials[materialIndex];
        output << "    {\"index\": " << material.materialIndex
               << ", \"name\": \"" << json_escape(material.name) << "\""
               << ", \"livePolygonParity\": " << (material.livePolygonParity ? "true" : "false")
               << ", \"cost\": {\"minimumTextureSamples\": "
               << material.cost.minimumTextureSamplesPerFragment
               << ", \"maximumTextureSamples\": "
               << material.cost.maximumTextureSamplesPerFragment
               << ", \"maximumHeightSamples\": "
               << material.cost.maximumHeightSamplesPerFragment
               << ", \"triplanarMultiplier\": " << material.cost.triplanarMultiplier
               << ", \"requiresTangents\": " << (material.cost.requiresTangents ? "true" : "false")
               << ", \"requiresUv1\": " << (material.cost.requiresUv1 ? "true" : "false")
               << ", \"requiresTransparentSorting\": "
               << (material.cost.requiresTransparentSorting ? "true" : "false")
               << ", \"tier\": \"" << json_escape(material.cost.tier) << "\"},\n"
               << "     \"features\": [";
        for (std::size_t featureIndex = 0; featureIndex < material.features.size(); ++featureIndex) {
            const MaterialFeatureCapability& feature = material.features[featureIndex];
            if (featureIndex != 0U) output << ',';
            output << "{\"name\": \"" << json_escape(material_feature_label(feature.feature))
                   << "\", \"cpuReference\": " << (feature.cpuReference ? "true" : "false")
                   << ", \"livePolygonMain\": " << (feature.livePolygonMain ? "true" : "false")
                   << ", \"shadowCaster\": " << (feature.shadowCaster ? "true" : "false")
                   << ", \"visualOnly\": " << (feature.visualOnly ? "true" : "false")
                   << ", \"geometryAffecting\": " << (feature.geometryAffecting ? "true" : "false")
                   << ", \"collisionAffecting\": " << (feature.collisionAffecting ? "true" : "false")
                   << ", \"note\": \"" << json_escape(feature.note) << "\"}";
        }
        output << "],\n     \"diagnostics\": [";
        for (std::size_t diagnosticIndex = 0; diagnosticIndex < material.diagnostics.size(); ++diagnosticIndex) {
            const MaterialAuthoringDiagnostic& diagnostic = material.diagnostics[diagnosticIndex];
            if (diagnosticIndex != 0U) output << ',';
            output << "{\"severity\": \""
                   << material_diagnostic_severity_label(diagnostic.severity)
                   << "\", \"code\": \"" << json_escape(diagnostic.code)
                   << "\", \"message\": \"" << json_escape(diagnostic.message) << "\"}";
        }
        output << "]}" << (materialIndex + 1U == report.materials.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"diagnostics\": [";
    for (std::size_t index = 0; index < report.diagnostics.size(); ++index) {
        const MaterialAuthoringDiagnostic& diagnostic = report.diagnostics[index];
        if (index != 0U) output << ',';
        output << "{\"severity\": \"" << material_diagnostic_severity_label(diagnostic.severity)
               << "\", \"code\": \"" << json_escape(diagnostic.code)
               << "\", \"materialIndex\": " << diagnostic.materialIndex
               << ", \"message\": \"" << json_escape(diagnostic.message) << "\"}";
    }
    output << "]\n}\n";
    return output.str();
}

bool write_material_authoring_report(const std::filesystem::path& path,
                                     const PolygonMaterialAuthoringReport& report,
                                     bool json,
                                     std::string* error) {
    try {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not open material report output");
        output << (json ? format_material_authoring_report_json(report)
                        : format_material_authoring_report_text(report));
        if (!output) throw std::runtime_error("could not write material report output");
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

} // namespace dve
