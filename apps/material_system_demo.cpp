#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <string>

#include "dve/gpu_material_buffer.hpp"
#include "dve/gpu_material_parameter_collection.hpp"
#include "dve/master_material.hpp"

namespace {
using namespace dve;

MasterMaterial make_rust_master() {
    MasterMaterial master;
    master.name = "RustLayer";
    master.parameters.scalars = {
        {std::string(kMetallicParam), 0.08F, 0.0F, 1.0F},
        {std::string(kRoughnessParam), 0.92F, 0.0F, 1.0F},
        {std::string(kSpecularParam), 0.32F, 0.0F, 1.0F},
    };
    master.parameters.vectors = {
        {std::string(kBaseColorParam), {0.58F, 0.10F, 0.025F, 1.0F}},
    };
    return master;
}

MasterMaterial make_clear_coat_master() {
    MasterMaterial master;
    master.name = "PaintedClearCoat";
    master.shadingModel = MaterialShadingModel::ClearCoat;
    master.parameters.scalars = {
        {std::string(kMetallicParam), 0.72F, 0.0F, 1.0F},
        {std::string(kRoughnessParam), 0.34F, 0.0F, 1.0F},
        {std::string(kSpecularParam), 0.50F, 0.0F, 1.0F},
        {std::string(kClearCoatParam), 0.90F, 0.0F, 1.0F},
        {std::string(kClearCoatRoughnessParam), 0.08F, 0.0F, 1.0F},
    };
    master.parameters.vectors = {
        {std::string(kBaseColorParam), {0.12F, 0.32F, 0.82F, 1.0F}},
    };
    master.globalScalars.push_back({std::string(kRoughnessParam), "RoughnessScale", MaterialGlobalCombine::Multiply});
    master.globalVectors.push_back({std::string(kBaseColorParam), "TimeOfDayTint", MaterialGlobalCombine::Multiply});
    return master;
}

MasterMaterial make_foliage_master() {
    MasterMaterial master;
    master.name = "TwoSidedLeaf";
    master.shadingModel = MaterialShadingModel::TwoSidedFoliage;
    master.blendMode = MaterialBlendMode::Masked;
    master.parameters.scalars = {
        {std::string(kRoughnessParam), 0.68F, 0.0F, 1.0F},
        {std::string(kSpecularParam), 0.32F, 0.0F, 1.0F},
        {std::string(kFoliageTransmittanceParam), 0.72F, 0.0F, 1.0F},
        {std::string(kFoliageWrapParam), 0.38F, 0.0F, 1.0F},
    };
    master.parameters.vectors = {
        {std::string(kBaseColorParam), {0.13F, 0.42F, 0.08F, 1.0F}},
        {std::string(kFoliageColorParam), {0.42F, 0.78F, 0.20F, 1.0F}},
    };
    master.globalVectors.push_back({std::string(kBaseColorParam), "TimeOfDayTint", MaterialGlobalCombine::Multiply});
    return master;
}

void write_vec4(std::ostream& out, Float4 v) {
    out << '[' << v.x << ',' << v.y << ',' << v.z << ',' << v.w << ']';
}

void write_material(std::ostream& out, const VoxelMaterialDefinition& material) {
    out << "{\"base_color\":";
    write_vec4(out, material.baseColor);
    out << ",\"metallic\":" << material.metallic
        << ",\"roughness\":" << material.roughness
        << ",\"specular\":" << material.specular
        << ",\"clear_coat\":" << material.clearCoat
        << ",\"clear_coat_roughness\":" << material.clearCoatRoughness
        << ",\"foliage_transmittance\":" << material.foliageTransmittance
        << ",\"foliage_wrap\":" << material.foliageWrap
        << ",\"shading_model\":" << static_cast<unsigned>(material.shadingModel)
        << '}';
}

} // namespace

int main(int argc, char** argv) {
    using namespace dve;
    MaterialLibrary library;
    std::string error;

    const MasterMaterialId rustMaster = library.add_master(make_rust_master(), &error);
    const MasterMaterialId coatMaster = library.add_master(make_clear_coat_master(), &error);
    const MasterMaterialId foliageMaster = library.add_master(make_foliage_master(), &error);
    if (rustMaster == kInvalidMasterMaterialId || coatMaster == kInvalidMasterMaterialId ||
        foliageMaster == kInvalidMasterMaterialId) {
        std::cerr << "master creation failed: " << error << '\n';
        return 1;
    }

    MaterialInstance rust;
    rust.name = "Rust";
    rust.materialId = 10;
    rust.master = rustMaster;
    if (library.add_instance(rust, &error) == kInvalidMaterialInstanceId) {
        std::cerr << "rust instance failed: " << error << '\n';
        return 1;
    }

    MaterialInstance coat;
    coat.name = "RustOverPaint";
    coat.materialId = 11;
    coat.master = coatMaster;
    coat.layers.push_back({10, 0.35F, MaterialLayerBlendMode::Lerp, true});
    if (library.add_instance(coat, &error) == kInvalidMaterialInstanceId) {
        std::cerr << "coat instance failed: " << error << '\n';
        return 1;
    }

    MaterialInstance foliage;
    foliage.name = "WindLeaf";
    foliage.materialId = 12;
    foliage.master = foliageMaster;
    if (library.add_instance(foliage, &error) == kInvalidMaterialInstanceId) {
        std::cerr << "foliage instance failed: " << error << '\n';
        return 1;
    }

    const bool globalsOk =
        library.set_global_scalar("TimeSeconds", 42.0F, &error) &&
        library.set_global_scalar("WindStrength", 0.65F, &error) &&
        library.set_global_scalar("Wetness", 0.20F, &error) &&
        library.set_global_scalar("RoughnessScale", 0.75F, &error) &&
        library.set_global_vector("TimeOfDayTint", {1.0F, 0.72F, 0.48F, 1.0F}, &error) &&
        library.set_global_vector("WindDirection", {0.35F, 0.0F, 0.94F, 0.0F}, &error);
    if (!globalsOk) {
        std::cerr << "global update failed: " << error << '\n';
        return 1;
    }

    VoxelMaterialDefinition layer0;
    VoxelMaterialDefinition layer50;
    VoxelMaterialDefinition layer100;
    if (!library.set_runtime_layer_weight(11, 0, 0.0F, &error) || !library.resolved(11)) return 1;
    layer0 = *library.resolved(11);
    if (!library.set_runtime_layer_weight(11, 0, 0.5F, &error) || !library.resolved(11)) return 1;
    layer50 = *library.resolved(11);
    if (!library.set_runtime_layer_weight(11, 0, 1.0F, &error) || !library.resolved(11)) return 1;
    layer100 = *library.resolved(11);
    if (!library.set_runtime_layer_weight(11, 0, 0.35F, &error)) return 1;

    const VoxelMaterialDefinition* finalCoat = library.resolved(11);
    const VoxelMaterialDefinition* finalFoliage = library.resolved(12);
    if (!finalCoat || !finalFoliage) return 1;
    const GpuMaterialRecord coatGpu = pack_gpu_material_record(*finalCoat);
    const GpuMaterialRecord foliageGpu = pack_gpu_material_record(*finalFoliage);
    const GpuMaterialParameterCollection globalsGpu =
        pack_gpu_material_parameter_collection(library.parameter_collection());

    std::filesystem::path collectionPath;
    if (argc >= 2) {
        collectionPath = std::filesystem::path(argv[1]).parent_path() / "unreal_material_globals.dvematparams";
        if (!library.save_parameter_collection(collectionPath, &error)) {
            std::cerr << "cannot save material parameter collection: " << error << '\n';
            return 1;
        }
        const auto reloaded = MaterialParameterCollection::load(collectionPath, &error);
        if (!reloaded || reloaded->scalar_count() != library.parameter_collection().scalar_count() ||
            reloaded->vector_count() != library.parameter_collection().vector_count()) {
            std::cerr << "material parameter collection reload failed: " << error << '\n';
            return 1;
        }
    }

    std::ofstream file;
    std::ostream* output = &std::cout;
    if (argc >= 2) {
        const std::filesystem::path path(argv[1]);
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        file.open(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "cannot open output: " << path << '\n';
            return 1;
        }
        output = &file;
    }

    std::ostream& out = *output;
    out << std::fixed << std::setprecision(6);
    out << "{\n  \"format\": \"DVE Unreal material feature evidence v1\",\n";
    out << "  \"collection_asset\": \"unreal_material_globals.dvematparams\",\n";
    out << "  \"material_parameter_collection\": {\"scalar_count\":" << globalsGpu.scalarCount
        << ",\"vector_count\":" << globalsGpu.vectorCount
        << ",\"time_seconds\":" << globalsGpu.scalarGroups[0].x
        << ",\"wind_strength\":" << globalsGpu.scalarGroups[0].y
        << ",\"wetness\":" << globalsGpu.scalarGroups[0].z
        << ",\"time_of_day_tint\":";
    write_vec4(out, library.parameter_collection().vector_at(kMpcTimeOfDayTintSlot));
    out << "},\n  \"clear_coat_layer_sweep\": {\"weight_0\":";
    write_material(out, layer0);
    out << ",\"weight_0_5\":";
    write_material(out, layer50);
    out << ",\"weight_1\":";
    write_material(out, layer100);
    out << "},\n  \"final_clear_coat\":";
    write_material(out, *finalCoat);
    out << ",\n  \"final_foliage\":";
    write_material(out, *finalFoliage);
    out << ",\n  \"gpu_contract\": {\"clear_coat_shading_model\":" << coatGpu.shadingModel
        << ",\"clear_coat_amount\":" << coatGpu.clearCoat
        << ",\"foliage_shading_model\":" << foliageGpu.shadingModel
        << ",\"foliage_transmittance\":" << foliageGpu.foliageTransmittance
        << "}\n}\n";
    return 0;
}
