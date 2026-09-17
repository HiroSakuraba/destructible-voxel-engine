#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "dve/gpu_material_buffer.hpp"

namespace {

int failures = 0;

#define CHECK(...)                                                                        \
    do {                                                                                  \
        if (!(__VA_ARGS__)) {                                                             \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n"; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

using namespace dve;

void test_record_size_and_field_count() {
    CHECK(sizeof(GpuMaterialRecord) == 92);
    CHECK(sizeof(GpuMaterialRecord) % 4 == 0);
}

void test_pack_field_values_and_order() {
    VoxelMaterialDefinition definition;
    definition.baseColor = {0.1F, 0.2F, 0.3F, 0.4F};
    definition.emissive = {0.5F, 0.6F, 0.7F};
    definition.metallic = 0.8F;
    definition.roughness = 0.9F;
    definition.specular = 0.25F;
    definition.shadingModel = MaterialShadingModel::Subsurface;
    definition.blendMode = MaterialBlendMode::Translucent;
    definition.subsurfaceScatterDistanceMeters = 0.05F;
    definition.subsurfaceColor = {0.11F, 0.22F, 0.33F};
    definition.clearCoat = 0.85F;
    definition.clearCoatRoughness = 0.12F;
    definition.foliageColor = {0.15F, 0.65F, 0.25F};
    definition.foliageTransmittance = 0.7F;
    definition.foliageWrap = 0.4F;

    const GpuMaterialRecord record = pack_gpu_material_record(definition);
    CHECK(record.baseColorR == 0.1F && record.baseColorG == 0.2F && record.baseColorB == 0.3F && record.baseColorA == 0.4F);
    CHECK(record.emissiveR == 0.5F && record.emissiveG == 0.6F && record.emissiveB == 0.7F);
    CHECK(record.metallic == 0.8F);
    CHECK(record.roughness == 0.9F);
    CHECK(record.specular == 0.25F);
    CHECK(record.shadingModel == 3U); // Subsurface, matching material_table.hlsli's kShadingModelSubsurface
    CHECK(record.blendMode == 2U);    // Translucent, matching kBlendModeTranslucent
    CHECK(record.subsurfaceScatterDistanceMeters == 0.05F);
    CHECK(record.subsurfaceColorR == 0.11F && record.subsurfaceColorG == 0.22F && record.subsurfaceColorB == 0.33F);
    CHECK(record.clearCoat == 0.85F && record.clearCoatRoughness == 0.12F);
    CHECK(record.foliageColorR == 0.15F && record.foliageColorG == 0.65F && record.foliageColorB == 0.25F);
    CHECK(record.foliageTransmittance == 0.7F && record.foliageWrap == 0.4F);

    // Byte offset of each field, checked directly: this is what actually guards against the
    // HLSL and C++ struct silently drifting out of field-order agreement, which sizeof() alone
    // cannot catch (two 64-byte layouts with the same field types in a different order would
    // both pass the size check and both be wrong).
    CHECK(offsetof(GpuMaterialRecord, baseColorR) == 0);
    CHECK(offsetof(GpuMaterialRecord, baseColorG) == 4);
    CHECK(offsetof(GpuMaterialRecord, baseColorB) == 8);
    CHECK(offsetof(GpuMaterialRecord, baseColorA) == 12);
    CHECK(offsetof(GpuMaterialRecord, emissiveR) == 16);
    CHECK(offsetof(GpuMaterialRecord, emissiveG) == 20);
    CHECK(offsetof(GpuMaterialRecord, emissiveB) == 24);
    CHECK(offsetof(GpuMaterialRecord, metallic) == 28);
    CHECK(offsetof(GpuMaterialRecord, roughness) == 32);
    CHECK(offsetof(GpuMaterialRecord, specular) == 36);
    CHECK(offsetof(GpuMaterialRecord, shadingModel) == 40);
    CHECK(offsetof(GpuMaterialRecord, blendMode) == 44);
    CHECK(offsetof(GpuMaterialRecord, subsurfaceScatterDistanceMeters) == 48);
    CHECK(offsetof(GpuMaterialRecord, subsurfaceColorR) == 52);
    CHECK(offsetof(GpuMaterialRecord, subsurfaceColorG) == 56);
    CHECK(offsetof(GpuMaterialRecord, subsurfaceColorB) == 60);
    CHECK(offsetof(GpuMaterialRecord, clearCoat) == 64);
    CHECK(offsetof(GpuMaterialRecord, clearCoatRoughness) == 68);
    CHECK(offsetof(GpuMaterialRecord, foliageColorR) == 72);
    CHECK(offsetof(GpuMaterialRecord, foliageColorG) == 76);
    CHECK(offsetof(GpuMaterialRecord, foliageColorB) == 80);
    CHECK(offsetof(GpuMaterialRecord, foliageTransmittance) == 84);
    CHECK(offsetof(GpuMaterialRecord, foliageWrap) == 88);
}

void test_shading_model_and_blend_mode_encoding() {
    VoxelMaterialDefinition definition;
    definition.shadingModel = MaterialShadingModel::StandardPBR;
    definition.blendMode = MaterialBlendMode::Opaque;
    CHECK(pack_gpu_material_record(definition).shadingModel == 0U);
    CHECK(pack_gpu_material_record(definition).blendMode == 0U);

    definition.shadingModel = MaterialShadingModel::Unlit;
    definition.blendMode = MaterialBlendMode::Masked;
    CHECK(pack_gpu_material_record(definition).shadingModel == 1U);
    CHECK(pack_gpu_material_record(definition).blendMode == 1U);

    definition.shadingModel = MaterialShadingModel::Emissive;
    CHECK(pack_gpu_material_record(definition).shadingModel == 2U);
    definition.shadingModel = MaterialShadingModel::Subsurface;
    CHECK(pack_gpu_material_record(definition).shadingModel == 3U);
    definition.shadingModel = MaterialShadingModel::TwoSidedFoliage;
    CHECK(pack_gpu_material_record(definition).shadingModel == 4U);
    definition.shadingModel = MaterialShadingModel::ClearCoat;
    CHECK(pack_gpu_material_record(definition).shadingModel == 5U);
}

void test_build_table_from_library() {
    MaterialLibrary library;
    MasterMaterial master;
    master.name = "Chrome";
    master.parameters.scalars.push_back({std::string(kMetallicParam), 1.0F, 0.0F, 1.0F});
    master.parameters.scalars.push_back({std::string(kRoughnessParam), 0.1F, 0.0F, 1.0F});
    master.parameters.vectors.push_back({std::string(kBaseColorParam), {0.9F, 0.9F, 0.95F, 1.0F}});
    const MasterMaterialId masterId = library.add_master(master);

    MaterialInstance instance;
    instance.name = "PolishedChrome";
    instance.materialId = 42;
    instance.master = masterId;
    std::string error;
    const MaterialInstanceId instanceId = library.add_instance(instance, &error);
    if (instanceId == kInvalidMaterialInstanceId) std::cerr << "add_instance failed: " << error << "\n";
    CHECK(instanceId != kInvalidMaterialInstanceId);

    const GpuMaterialTable table = build_gpu_material_table(library);
    CHECK(table.size() == 256);
    CHECK(table[42].metallic == 1.0F);
    CHECK(std::fabs(table[42].roughness - 0.1F) < 1.0e-6F);
    CHECK(table[42].baseColorR == 0.9F);

    // Every other slot must have a safe, visibly-distinguishable-from-black fallback, not a
    // zeroed (degenerate roughness=0) record.
    for (std::uint32_t id = 0; id <= 255; ++id) {
        if (id == 42) continue;
        CHECK(std::fabs(table[id].roughness - kStandardSurfaceRoughness) < 1.0e-6F);
        CHECK(std::fabs(table[id].baseColorR - 0.214F) < 1.0e-6F);
        CHECK(std::fabs(table[id].specular - kStandardSurfaceSpecular) < 1.0e-6F);
    }
}

void test_runtime_override_reflected_in_packed_table() {
    MaterialLibrary library;
    MasterMaterial master;
    master.name = "Lamp";
    master.parameters.scalars.push_back({std::string(kRoughnessParam), 0.7F, 0.0F, 1.0F});
    const MasterMaterialId masterId = library.add_master(master);
    MaterialInstance instance;
    instance.name = "LampBulb";
    instance.materialId = 5;
    instance.master = masterId;
    CHECK(library.add_instance(instance) != kInvalidMaterialInstanceId);

    std::string setError;
    CHECK(library.set_runtime_scalar(5, kRoughnessParam, 0.2F, &setError));
    if (!setError.empty()) std::cerr << "  (unexpected) set_runtime_scalar error: " << setError << "\n";
    const GpuMaterialTable table = build_gpu_material_table(library);
    CHECK(std::fabs(table[5].roughness - 0.2F) < 1.0e-6F);
}


std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void test_hlsl_material_contract_sources_are_present_and_aligned() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::string table = read_text_file(root / "shaders/common/material_record.hlsli");
    const std::string globals = read_text_file(root / "shaders/common/material_parameter_collection.hlsli");
    const std::string lighting = read_text_file(root / "shaders/common/pbr_lighting.hlsli");
    const std::string shade = read_text_file(root / "shaders/shade_primary.hlsl");
    CHECK(!table.empty() && !globals.empty() && !lighting.empty() && !shade.empty());
    CHECK(table.find("static const uint kShadingModelTwoSidedFoliage = 4u") != std::string::npos);
    CHECK(table.find("static const uint kShadingModelClearCoat = 5u") != std::string::npos);
    CHECK(table.find("float clearCoat;") < table.find("float clearCoatRoughness;"));
    CHECK(table.find("float clearCoatRoughness;") < table.find("float foliageColorR;"));
    CHECK(table.find("float foliageTransmittance;") < table.find("float foliageWrap;"));
    CHECK(globals.find("cbuffer MaterialParameterCollectionConstants : register(b4)") != std::string::npos);
    CHECK(globals.find("static const uint kMpcTimeOfDayTint = 0u") != std::string::npos);
    CHECK(lighting.find("ShadeClearCoatDirect") != std::string::npos);
    CHECK(lighting.find("ShadeTwoSidedFoliageDirect") != std::string::npos);
    CHECK(shade.find("kShadingModelClearCoat") != std::string::npos);
    CHECK(shade.find("kShadingModelTwoSidedFoliage") != std::string::npos);
    CHECK(shade.find("MaterialGlobalVector(kMpcTimeOfDayTint)") != std::string::npos);
}

void test_cooked_material_layers_are_resolved_before_pack() {
    std::vector<VoxelMaterialDefinition> materials(3);
    materials[1].name = "Steel";
    materials[1].metallic = 1.0F;
    materials[1].roughness = 0.2F;
    materials[1].baseColor = {0.3F, 0.3F, 0.35F, 1.0F};
    materials[2].name = "Rust";
    materials[2].metallic = 0.0F;
    materials[2].roughness = 1.0F;
    materials[2].baseColor = {0.6F, 0.2F, 0.05F, 1.0F};
    materials[1].layers.push_back({2, 0.5F, MaterialLayerBlendMode::Lerp, true});
    std::string error;
    const auto table = build_gpu_material_table(materials, &error);
    CHECK(table.has_value());
    if (table) {
        CHECK(std::fabs((*table)[1].metallic - 0.5F) < 1.0e-6F);
        CHECK(std::fabs((*table)[1].roughness - 0.6F) < 1.0e-6F);
        CHECK(std::fabs((*table)[1].baseColorR - 0.45F) < 1.0e-6F);
    }

    materials[2].layers.push_back({1, 0.5F, MaterialLayerBlendMode::Lerp, true});
    error.clear();
    CHECK(!build_gpu_material_table(materials, &error).has_value());
    CHECK(!error.empty());
}

} // namespace

int main() {
    test_record_size_and_field_count();
    test_pack_field_values_and_order();
    test_shading_model_and_blend_mode_encoding();
    test_build_table_from_library();
    test_runtime_override_reflected_in_packed_table();
    test_hlsl_material_contract_sources_are_present_and_aligned();
    test_cooked_material_layers_are_resolved_before_pack();

    if (failures == 0) {
        std::cout << "dve_gpu_material_buffer_tests: PASS\n";
        return 0;
    }
    std::cerr << "dve_gpu_material_buffer_tests: " << failures << " FAILURE(S)\n";
    return 1;
}
