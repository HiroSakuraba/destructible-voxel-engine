#include "dve/material_authoring.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace dve;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

CookedPolygonAsset make_advanced_asset() {
    CookedPolygonAsset asset;
    asset.objectId = 173U;

    VoxelMaterialDefinition advanced;
    advanced.name = "Advanced Authoring Material";
    advanced.shadingModel = MaterialShadingModel::ClearCoat;
    advanced.blendMode = MaterialBlendMode::Translucent;
    advanced.transparent = true;
    advanced.clearCoat = 0.75F;
    advanced.layers.push_back({1U, 0.35F, MaterialLayerBlendMode::Multiply, true});
    asset.materials.push_back(advanced);

    VoxelMaterialDefinition layer;
    layer.name = "Layer Source";
    asset.materials.push_back(layer);

    const auto add_texture = [&](std::string name, std::array<std::uint8_t, 4> rgba) {
        PolygonImage image;
        image.name = name;
        image.mimeType = "image/raw";
        image.width = 1U;
        image.height = 1U;
        image.rgba8.assign(rgba.begin(), rgba.end());
        asset.images.push_back(std::move(image));
        asset.samplers.push_back({});
        asset.textures.push_back({name, static_cast<std::uint32_t>(asset.images.size() - 1U),
                                  static_cast<std::uint32_t>(asset.samplers.size() - 1U)});
        return static_cast<std::uint32_t>(asset.textures.size() - 1U);
    };

    PolygonMaterialBinding binding;
    binding.baseColor.texture = add_texture("base", {180U, 120U, 80U, 200U});
    binding.normal.texture = add_texture("normal", {128U, 128U, 255U, 255U});
    binding.normal.colorSpace = PolygonTextureColorSpace::Srgb; // intentionally suspicious
    binding.detailBaseColor.texture = add_texture("detail", {140U, 140U, 140U, 255U});
    binding.detailBaseColor.texcoord = 1U;
    binding.height.texture = add_texture("height", {170U, 170U, 170U, 255U});
    binding.mapping.mappingMode = MaterialMappingMode::UV1;
    binding.mapping.detailColorStrength = 1.0F;
    binding.mapping.heightMode = HeightMappingMode::ParallaxOcclusion;
    binding.mapping.minimumHeightSteps = 8U;
    binding.mapping.maximumHeightSteps = 16U;
    binding.mapping.refinementSteps = 3U;
    PolygonMaterialLayerBinding layerBinding;layerBinding.semantic=MaterialLayerSemantic::Rust;
    layerBinding.mask.texture=binding.detailBaseColor.texture;layerBinding.height.texture=binding.height.texture;
    layerBinding.heightBlendStrength=0.5F;binding.layers.push_back(layerBinding);
    asset.materialBindings.push_back(binding);
    asset.materialBindings.push_back({});

    asset.vertices = {
        {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F},
         {0.0F, 0.0F}, {1.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F}},
        {{ 1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F},
         {1.0F, 0.0F}, {1.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F}},
        {{ 0.0F,  1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F},
         {0.5F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F}},
    };
    asset.indices = {0U, 1U, 2U};
    asset.submeshes.push_back({"triangle", 0U, 3U, 0U});
    asset.bounds = {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}};
    asset.contentHash = polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)), "advanced test asset is invalid");
    return asset;
}

void test_authoring_report() {
    const CookedPolygonAsset asset = make_advanced_asset();
    const PolygonMaterialAuthoringReport report = analyze_polygon_material_authoring(asset);
    require(report.assetValid, "valid asset was rejected by material authoring analysis");
    require(report.materials.size() == 2U, "material report count mismatch");
    require(report.warningCount >= 6U, "advanced material did not expose expected warnings");

    const PolygonMaterialAuthoringEntry& material = report.materials.front();
    require(!material.livePolygonParity, "reference-only material incorrectly claims live parity");
    require(material.cost.minimumTextureSamplesPerFragment == 2U,
            "minimum sample estimate drifted");
    require(material.cost.maximumHeightSamplesPerFragment == 19U,
            "POM sample estimate drifted");
    require(material.cost.maximumTextureSamplesPerFragment == 24U,
            "maximum layered sample estimate drifted");
    require(material.cost.requiresTangents && material.cost.requiresUv1 &&
            material.cost.requiresTransparentSorting,
            "material execution requirements were not detected");
    require(material.cost.tier == "very-high", "material cost tier is incorrect");

    const auto has_code = [&](std::string_view code) {
        for (const MaterialAuthoringDiagnostic& diagnostic : material.diagnostics) {
            if (diagnostic.code == code) return true;
        }
        return false;
    };
    require(has_code("LIVE_SHADING_MODEL_REFERENCE_ONLY"), "clear-coat parity warning missing");
    require(has_code("LIVE_TRANSLUCENCY_UNAVAILABLE"), "translucency warning missing");
    require(has_code("LIVE_UV1_REFERENCE_ONLY"), "UV1 warning missing");
    require(has_code("UV1_DEGENERATE"), "degenerate UV1 warning missing");
    require(has_code("DATA_TEXTURE_MARKED_SRGB"), "data color-space warning missing");
    require(has_code("PARALLAX_VISUAL_ONLY"), "visual-only POM warning missing");
    require(has_code("LIVE_LAYER_MASKS_REFERENCE_ONLY"), "per-pixel layer parity warning missing");

    const auto capabilities=material_editor_capabilities();
    require(capabilities.size()>=5U,"global editor capabilities are incomplete");
    require(std::any_of(capabilities.begin(),capabilities.end(),[](const MaterialFeatureCapability& feature){
        return feature.feature==MaterialAuthoringFeature::ProjectedDecals&&feature.cpuReference&&!feature.livePolygonMain;
    }),"projected-decal capability row missing");

    const std::string text = format_material_authoring_report_text(report);
    const std::string json = format_material_authoring_report_json(report);
    require(text.find("estimated_samples: 2..24") != std::string::npos,
            "text report omitted cost estimate");
    require(json.find("dve.material-authoring-report.v1") != std::string::npos &&
            json.find("\"maximumTextureSamples\": 24") != std::string::npos,
            "JSON report omitted schema or cost estimate");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dve_material_report_" + std::to_string(stamp) + ".json");
    std::string error;
    require(write_material_authoring_report(path, report, true, &error), error.c_str());
    require(std::filesystem::file_size(path) > 1000U, "written material report is unexpectedly small");
    std::filesystem::remove(path);
}

void test_invalid_asset_report() {
    const PolygonMaterialAuthoringReport report = analyze_polygon_material_authoring({});
    require(!report.assetValid && report.has_errors(), "invalid asset did not produce an error report");
    require(report.materials.empty(), "invalid asset produced material entries");
    require(report.diagnostics.front().code == "ASSET_INVALID", "invalid asset diagnostic code drifted");
}

} // namespace

int main() {
    try {
        test_authoring_report();
        test_invalid_asset_report();
        std::cout << "material authoring tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "material authoring tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
