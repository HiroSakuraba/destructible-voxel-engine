#include <cmath>
#include <filesystem>
#include <iostream>

#include "dve/master_material.hpp"

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

[[nodiscard]] MasterMaterial make_standard_master(std::string name) {
    MasterMaterial master;
    master.name = std::move(name);
    master.shadingModel = MaterialShadingModel::StandardPBR;
    master.parameters.vectors.push_back({std::string(kBaseColorParam), {0.8F, 0.8F, 0.8F, 1.0F}});
    master.parameters.vectors.push_back({std::string(kEmissiveParam), {0.0F, 0.0F, 0.0F, 0.0F}});
    master.parameters.scalars.push_back({std::string(kMetallicParam), 0.0F, 0.0F, 1.0F});
    master.parameters.scalars.push_back({std::string(kRoughnessParam), 0.8F, 0.0F, 1.0F});
    master.parameters.switches.push_back({std::string(kTransparentParam), false});
    return master;
}


void test_standard_surface_factory() {
    MasterMaterial master = make_standard_surface_master_material();
    CHECK(master.name == kStandardSurfaceMasterName);
    CHECK(master.shadingModel == MaterialShadingModel::StandardPBR);
    CHECK(master.blendMode == MaterialBlendMode::Opaque);
    CHECK(master.parameters.find_vector(kBaseColorParam) != nullptr);
    CHECK(master.parameters.find_scalar(kMetallicParam) != nullptr);
    CHECK(master.parameters.find_scalar(kRoughnessParam) != nullptr);
    CHECK(master.parameters.find_scalar(kSpecularParam) != nullptr);
    CHECK(std::fabs(master.parameters.find_vector(kBaseColorParam)->defaultValue.x - 0.214F) < 1.0e-6F);
    CHECK(std::fabs(master.parameters.find_scalar(kRoughnessParam)->defaultValue - kStandardSurfaceRoughness) < 1.0e-6F);
    CHECK(std::fabs(master.parameters.find_scalar(kSpecularParam)->defaultValue - kStandardSurfaceSpecular) < 1.0e-6F);
}

void test_basic_master_and_instance() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    MasterMaterial master = make_standard_master("Wood");
    master.id = 1;
    masters[1] = master;

    MaterialInstance instance;
    instance.id = 10;
    instance.name = "OakPlank";
    instance.materialId = 5;
    instance.master = 1;
    instance.densityKilogramsPerCubicMeter = 700.0F;
    instance.structuralStrength = 0.6F;

    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(result.success());
    if (!result.success()) std::cerr << "  detail: " << result.errorDetail << "\n";
    CHECK(result.definition.name == "OakPlank");
    CHECK(result.definition.baseColor.x == 0.8F && result.definition.baseColor.w == 1.0F);
    CHECK(result.definition.roughness == 0.8F);
    CHECK(result.definition.metallic == 0.0F);
    CHECK(!result.definition.transparent);
    CHECK(result.definition.densityKilogramsPerCubicMeter == 700.0F);
    CHECK(result.definition.structuralStrength == 0.6F);
}

void test_instance_overrides() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("Wood");
    masters[1].id = 1;

    MaterialInstance instance;
    instance.id = 10;
    instance.name = "DarkOak";
    instance.materialId = 5;
    instance.master = 1;
    instance.vectorOverrides[std::string(kBaseColorParam)] = {0.2F, 0.1F, 0.05F, 1.0F};
    instance.scalarOverrides[std::string(kRoughnessParam)] = 0.5F;

    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(result.success());
    CHECK(std::fabs(result.definition.baseColor.x - 0.2F) < 1.0e-6F);
    CHECK(std::fabs(result.definition.roughness - 0.5F) < 1.0e-6F);
    // Metallic was not overridden: must still be the master's default.
    CHECK(result.definition.metallic == 0.0F);
}

void test_instance_of_instance_chain() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("Metal");
    masters[1].id = 1;

    MaterialInstance grandparent;
    grandparent.id = 10;
    grandparent.name = "Steel";
    grandparent.materialId = 5;
    grandparent.master = 1;
    grandparent.scalarOverrides[std::string(kMetallicParam)] = 1.0F;
    grandparent.scalarOverrides[std::string(kRoughnessParam)] = 0.4F;

    MaterialInstance parent;
    parent.id = 11;
    parent.name = "RustySteel";
    parent.materialId = 6;
    parent.parent = 10;
    parent.vectorOverrides[std::string(kBaseColorParam)] = {0.4F, 0.2F, 0.1F, 1.0F};
    // Deliberately does not touch Roughness: should inherit grandparent's 0.4, not the
    // master's original default.

    MaterialInstance child;
    child.id = 12;
    child.name = "PolishedRustySteel";
    child.materialId = 7;
    child.parent = 11;
    child.scalarOverrides[std::string(kRoughnessParam)] = 0.1F; // overrides the grandparent's value

    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{
        {10, grandparent}, {11, parent}, {12, child}};

    const MaterialResolveResult parentResult = resolve_material_instance(11, masters, instances);
    CHECK(parentResult.success());
    if (!parentResult.success()) std::cerr << "  detail: " << parentResult.errorDetail << "\n";
    CHECK(std::fabs(parentResult.definition.baseColor.x - 0.4F) < 1.0e-6F); // own override
    CHECK(parentResult.definition.metallic == 1.0F);                        // inherited from grandparent
    CHECK(std::fabs(parentResult.definition.roughness - 0.4F) < 1.0e-6F);   // inherited from grandparent

    const MaterialResolveResult childResult = resolve_material_instance(12, masters, instances);
    CHECK(childResult.success());
    CHECK(std::fabs(childResult.definition.baseColor.x - 0.4F) < 1.0e-6F);  // inherited from parent
    CHECK(childResult.definition.metallic == 1.0F);                         // inherited from grandparent
    CHECK(std::fabs(childResult.definition.roughness - 0.1F) < 1.0e-6F);   // own override wins
    // Physics fields come from the leaf (child) instance, not any ancestor.
    CHECK(childResult.definition.name == "PolishedRustySteel");
}

void test_cycle_detection() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("Loop");
    masters[1].id = 1;

    MaterialInstance a;
    a.id = 10; a.name = "A"; a.materialId = 5; a.master = 1; a.parent = 11;
    MaterialInstance b;
    b.id = 11; b.name = "B"; b.materialId = 6; b.parent = 10; // points back to a

    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, a}, {11, b}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::CyclicInheritance);
}

void test_unknown_master() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters; // empty
    MaterialInstance instance;
    instance.id = 10; instance.name = "Orphan"; instance.materialId = 5; instance.master = 99;
    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::UnknownMaster);
}

void test_unknown_parent() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("M");
    masters[1].id = 1;
    MaterialInstance instance;
    instance.id = 10; instance.name = "Dangling"; instance.materialId = 5; instance.parent = 999;
    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::UnknownParent);
}

void test_unknown_instance() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    std::unordered_map<MaterialInstanceId, MaterialInstance> instances;
    const MaterialResolveResult result = resolve_material_instance(123, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::UnknownInstance);
}

void test_inconsistent_master() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("A");
    masters[1].id = 1;
    masters[2] = make_standard_master("B");
    masters[2].id = 2;

    MaterialInstance parent;
    parent.id = 10; parent.name = "Parent"; parent.materialId = 5; parent.master = 1;
    MaterialInstance child;
    child.id = 11; child.name = "Child"; child.materialId = 6; child.parent = 10; child.master = 2; // different master

    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, parent}, {11, child}};
    const MaterialResolveResult result = resolve_material_instance(11, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::InconsistentMaster);
}

void test_unknown_parameter_rejected() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("M");
    masters[1].id = 1;
    MaterialInstance instance;
    instance.id = 10; instance.name = "Typo"; instance.materialId = 5; instance.master = 1;
    instance.scalarOverrides["Roughnes"] = 0.5F; // misspelled
    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::UnknownParameter);
}

void test_scalar_out_of_range_rejected() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    masters[1] = make_standard_master("M");
    masters[1].id = 1;
    MaterialInstance instance;
    instance.id = 10; instance.name = "TooRough"; instance.materialId = 5; instance.master = 1;
    instance.scalarOverrides[std::string(kRoughnessParam)] = 1.5F; // schema max is 1.0
    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(!result.success());
    CHECK(result.error == MaterialResolveErrorCode::ScalarOutOfRange);
}

void test_material_library_basic_flow() {
    MaterialLibrary library;
    std::string error;
    MasterMaterial master = make_standard_master("Fabric");
    const MasterMaterialId masterId = library.add_master(master, &error);
    if (masterId == kInvalidMasterMaterialId) std::cerr << "add_master failed: " << error << "\n";
    CHECK(masterId != kInvalidMasterMaterialId);

    MaterialInstance instance;
    instance.name = "RedFabric";
    instance.materialId = 3;
    instance.master = masterId;
    instance.vectorOverrides[std::string(kBaseColorParam)] = {0.8F, 0.1F, 0.1F, 1.0F};
    const MaterialInstanceId instanceId = library.add_instance(instance, &error);
    if (instanceId == kInvalidMaterialInstanceId) std::cerr << "add_instance failed: " << error << "\n";
    CHECK(instanceId != kInvalidMaterialInstanceId);

    const VoxelMaterialDefinition* resolved = library.resolved(3);
    CHECK(resolved != nullptr);
    if (resolved) CHECK(std::fabs(resolved->baseColor.x - 0.8F) < 1.0e-6F);

    CHECK(library.find_master_by_name("Fabric") == masterId);
    CHECK(library.find_instance_by_material_id(3) == instanceId);
    CHECK(!library.find_instance_by_material_id(99).has_value());
}

void test_material_library_rejects_duplicate_material_id() {
    MaterialLibrary library;
    MasterMaterial master = make_standard_master("M");
    const MasterMaterialId masterId = library.add_master(master);

    MaterialInstance first;
    first.name = "First";
    first.materialId = 9;
    first.master = masterId;
    CHECK(library.add_instance(first) != kInvalidMaterialInstanceId);

    MaterialInstance second;
    second.name = "Second";
    second.materialId = 9; // same slot
    second.master = masterId;
    std::string error;
    CHECK(library.add_instance(second, &error) == kInvalidMaterialInstanceId);
    CHECK(!error.empty());
}

void test_material_library_partial_failure_keeps_good_instances() {
    MaterialLibrary library;
    MasterMaterial master = make_standard_master("M");
    const MasterMaterialId masterId = library.add_master(master);

    MaterialInstance good;
    good.name = "Good";
    good.materialId = 1;
    good.master = masterId;
    const MaterialInstanceId goodId = library.add_instance(good);
    CHECK(goodId != kInvalidMaterialInstanceId);
    CHECK(library.resolved(1) != nullptr);

    MaterialInstance bad;
    bad.name = "Bad";
    bad.materialId = 2;
    bad.master = 12345; // unknown master
    std::string error;
    const MaterialInstanceId badId = library.add_instance(bad, &error);
    // add_instance itself only validates name/materialId-uniqueness, not resolvability, so
    // this should still be added but fail to resolve.
    CHECK(badId != kInvalidMaterialInstanceId);
    CHECK(library.resolved(2) == nullptr);
    CHECK(library.resolved(1) != nullptr); // the good one is unaffected by the bad one

    std::string resolveError;
    CHECK(!library.resolve_all(&resolveError));
    CHECK(resolveError.find("12345") != std::string::npos || !resolveError.empty());
    CHECK(library.resolved(1) != nullptr); // still fine after resolve_all reports overall failure

    // A pre-existing broken editor instance must not prevent a canonical global update from
    // reaching every material that was already valid.
    error.clear();
    CHECK(library.set_global_scalar("Wetness", 0.4F, &error));
    CHECK(error.empty());
    CHECK(library.resolved(1) != nullptr);
    CHECK(library.resolved(2) == nullptr);
    CHECK(library.set_runtime_scalar(1, kRoughnessParam, 0.25F, &error));
    CHECK(error.empty());
    CHECK(library.resolved(1) && std::fabs(library.resolved(1)->roughness - 0.25F) < 1.0e-6F);
}

void test_runtime_overrides() {
    MaterialLibrary library;
    MasterMaterial master = make_standard_master("Lava");
    const MasterMaterialId masterId = library.add_master(master);
    MaterialInstance instance;
    instance.name = "Lava01";
    instance.materialId = 4;
    instance.master = masterId;
    instance.vectorOverrides[std::string(kEmissiveParam)] = {1.0F, 0.3F, 0.0F, 0.0F};
    CHECK(library.add_instance(instance) != kInvalidMaterialInstanceId);

    const VoxelMaterialDefinition* before = library.resolved(4);
    CHECK(before != nullptr);
    if (before) CHECK(before->emissive.x == 1.0F);

    std::string error;
    const bool runtimeSetOk = library.set_runtime_scalar(4, kRoughnessParam, 0.2F, &error);
    if (!runtimeSetOk) std::cerr << "set_runtime_scalar failed: " << error << "\n";
    CHECK(runtimeSetOk);
    const VoxelMaterialDefinition* afterRuntime = library.resolved(4);
    CHECK(afterRuntime != nullptr);
    if (afterRuntime) CHECK(std::fabs(afterRuntime->roughness - 0.2F) < 1.0e-6F);

    // An unrelated library mutation (adding a second, independent instance) must not discard
    // the live runtime override on the first one: resolve_all() re-applies runtime overrides,
    // it does not clear them.
    MaterialInstance other;
    other.name = "Other";
    other.materialId = 8;
    other.master = masterId;
    CHECK(library.add_instance(other) != kInvalidMaterialInstanceId);
    const VoxelMaterialDefinition* stillOverridden = library.resolved(4);
    CHECK(stillOverridden != nullptr);
    if (stillOverridden) CHECK(std::fabs(stillOverridden->roughness - 0.2F) < 1.0e-6F);

    CHECK(library.clear_runtime_overrides(4));
    const VoxelMaterialDefinition* reverted = library.resolved(4);
    CHECK(reverted != nullptr);
    if (reverted) CHECK(reverted->roughness == 0.8F); // back to the master's default
}

void test_runtime_override_validation() {
    MaterialLibrary library;
    MasterMaterial master = make_standard_master("M");
    const MasterMaterialId masterId = library.add_master(master);
    MaterialInstance instance;
    instance.name = "Inst";
    instance.materialId = 6;
    instance.master = masterId;
    CHECK(library.add_instance(instance) != kInvalidMaterialInstanceId);

    std::string error;
    CHECK(!library.set_runtime_scalar(6, "NotAParam", 0.5F, &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!library.set_runtime_scalar(6, kRoughnessParam, 5.0F, &error)); // out of range
    CHECK(!error.empty());
    error.clear();
    CHECK(!library.set_runtime_scalar(250, kRoughnessParam, 0.5F, &error)); // no such material id
    CHECK(!error.empty());
}

void test_new_shading_fields_resolve_correctly() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    MasterMaterial master;
    master.id = 1;
    master.name = "Jade";
    master.shadingModel = MaterialShadingModel::Subsurface;
    master.blendMode = MaterialBlendMode::Masked;
    master.parameters.scalars.push_back({std::string(kSpecularParam), 0.6F, 0.0F, 1.0F});
    master.parameters.scalars.push_back({std::string(kSubsurfaceScatterDistanceParam), 0.05F, 0.0F, 10.0F});
    master.parameters.vectors.push_back({std::string(kSubsurfaceColorParam), {0.1F, 0.8F, 0.3F, 1.0F}});
    masters[1] = master;

    MaterialInstance instance;
    instance.id = 10;
    instance.name = "JadeStatue";
    instance.materialId = 9;
    instance.master = 1;
    instance.scalarOverrides[std::string(kSubsurfaceScatterDistanceParam)] = 0.08F;

    std::unordered_map<MaterialInstanceId, MaterialInstance> instances{{10, instance}};
    const MaterialResolveResult result = resolve_material_instance(10, masters, instances);
    CHECK(result.success());
    if (!result.success()) std::cerr << "  detail: " << result.errorDetail << "\n";
    CHECK(result.definition.shadingModel == MaterialShadingModel::Subsurface);
    CHECK(result.definition.blendMode == MaterialBlendMode::Masked);
    CHECK(std::fabs(result.definition.specular - 0.6F) < 1.0e-6F); // master default, not overridden
    CHECK(std::fabs(result.definition.subsurfaceScatterDistanceMeters - 0.08F) < 1.0e-6F); // instance override
    CHECK(std::fabs(result.definition.subsurfaceColor.y - 0.8F) < 1.0e-6F);
}


void test_material_parameter_collection_bindings_and_rollback() {
    MaterialLibrary library;
    MasterMaterial master = make_standard_master("GloballyTinted");
    master.globalScalars.push_back({std::string(kRoughnessParam), "RoughnessScale", MaterialGlobalCombine::Multiply});
    master.globalVectors.push_back({std::string(kBaseColorParam), "TimeOfDayTint", MaterialGlobalCombine::Multiply});
    const MasterMaterialId masterId = library.add_master(master);
    CHECK(masterId != kInvalidMasterMaterialId);

    MaterialInstance instance;
    instance.name = "GlobalSurface";
    instance.materialId = 20;
    instance.master = masterId;
    CHECK(library.add_instance(instance) != kInvalidMaterialInstanceId);

    std::string error;
    CHECK(library.set_global_scalar("RoughnessScale", 0.5F, &error));
    CHECK(library.set_global_vector("TimeOfDayTint", {0.5F, 1.0F, 0.25F, 1.0F}, &error));
    const VoxelMaterialDefinition* resolved = library.resolved(20);
    CHECK(resolved != nullptr);
    if (resolved) {
        CHECK(std::fabs(resolved->roughness - 0.4F) < 1.0e-6F);
        CHECK(std::fabs(resolved->baseColor.x - 0.4F) < 1.0e-6F);
        CHECK(std::fabs(resolved->baseColor.z - 0.2F) < 1.0e-6F);
    }

    // 2.0 would drive roughness 0.8*2 outside the schema max. The collection and material
    // must both remain at the previous valid value.
    error.clear();
    CHECK(!library.set_global_scalar("RoughnessScale", 2.0F, &error));
    CHECK(!error.empty());
    CHECK(std::fabs(*library.parameter_collection().scalar("RoughnessScale") - 0.5F) < 1.0e-6F);
    resolved = library.resolved(20);
    CHECK(resolved && std::fabs(resolved->roughness - 0.4F) < 1.0e-6F);
}

void test_material_layers_and_runtime_weight() {
    MaterialLibrary library;
    MasterMaterial steel = make_standard_master("Steel");
    const MasterMaterialId steelMaster = library.add_master(steel);
    MasterMaterial rust = make_standard_master("Rust");
    const MasterMaterialId rustMaster = library.add_master(rust);

    MaterialInstance rustInstance;
    rustInstance.name = "RustLayer";
    rustInstance.materialId = 21;
    rustInstance.master = rustMaster;
    rustInstance.scalarOverrides[std::string(kMetallicParam)] = 0.0F;
    rustInstance.scalarOverrides[std::string(kRoughnessParam)] = 1.0F;
    rustInstance.vectorOverrides[std::string(kBaseColorParam)] = {0.55F, 0.18F, 0.04F, 1.0F};
    CHECK(library.add_instance(rustInstance) != kInvalidMaterialInstanceId);

    MaterialInstance steelInstance;
    steelInstance.name = "RustySteel";
    steelInstance.materialId = 22;
    steelInstance.master = steelMaster;
    steelInstance.scalarOverrides[std::string(kMetallicParam)] = 1.0F;
    steelInstance.scalarOverrides[std::string(kRoughnessParam)] = 0.2F;
    steelInstance.vectorOverrides[std::string(kBaseColorParam)] = {0.3F, 0.32F, 0.35F, 1.0F};
    steelInstance.layers.push_back({21, 0.5F, MaterialLayerBlendMode::Lerp, true});
    CHECK(library.add_instance(steelInstance) != kInvalidMaterialInstanceId);

    const VoxelMaterialDefinition* blended = library.resolved(22);
    CHECK(blended != nullptr);
    if (blended) {
        CHECK(std::fabs(blended->metallic - 0.5F) < 1.0e-6F);
        CHECK(std::fabs(blended->roughness - 0.6F) < 1.0e-6F);
        CHECK(std::fabs(blended->baseColor.x - 0.425F) < 1.0e-6F);
        CHECK(blended->densityKilogramsPerCubicMeter == steelInstance.densityKilogramsPerCubicMeter);
    }

    std::string error;
    CHECK(library.set_runtime_layer_weight(22, 0, 1.0F, &error));
    blended = library.resolved(22);
    CHECK(blended && std::fabs(blended->metallic - 0.0F) < 1.0e-6F);
    CHECK(library.runtime_layer_weight(22, 0) == 1.0F);
    CHECK(library.clear_runtime_layer_weights(22));
    blended = library.resolved(22);
    CHECK(blended && std::fabs(blended->metallic - 0.5F) < 1.0e-6F);
}

void test_material_layer_cycle_is_rejected() {
    MaterialLibrary library;
    const MasterMaterialId master = library.add_master(make_standard_master("Layered"));
    MaterialInstance a;
    a.name = "A"; a.materialId = 30; a.master = master;
    const MaterialInstanceId aId = library.add_instance(a);
    MaterialInstance b;
    b.name = "B"; b.materialId = 31; b.master = master;
    b.layers.push_back({30, 0.5F, MaterialLayerBlendMode::Lerp, true});
    CHECK(library.add_instance(b) != kInvalidMaterialInstanceId);
    a.id = aId;
    a.layers.push_back({31, 0.5F, MaterialLayerBlendMode::Lerp, true});
    std::string error;
    CHECK(library.update_instance(a, &error)); // retained for editor repair, but unresolved
    error.clear();
    CHECK(!library.resolve_all(&error));
    CHECK(!error.empty());
    CHECK(library.resolved(30) == nullptr || library.resolved(31) == nullptr);
}

void test_material_library_collection_save_load_re_resolves() {
    MaterialLibrary source;
    MasterMaterial master = make_standard_master("GlobalRoughness");
    master.globalScalars.push_back({std::string(kRoughnessParam), "RoughnessScale", MaterialGlobalCombine::Multiply});
    const MasterMaterialId masterId = source.add_master(master);
    MaterialInstance instance;
    instance.name = "Bound"; instance.materialId = 44; instance.master = masterId;
    CHECK(source.add_instance(instance) != kInvalidMaterialInstanceId);
    CHECK(source.set_global_scalar("RoughnessScale", 0.5F));
    CHECK(source.resolved(44) && std::fabs(source.resolved(44)->roughness - 0.4F) < 1.0e-6F);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dve_material_library_globals";
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "world.dvematparams";
    std::string error;
    CHECK(source.save_parameter_collection(path, &error));

    MaterialLibrary destination;
    const MasterMaterialId destinationMaster = destination.add_master(master);
    instance.master = destinationMaster;
    CHECK(destination.add_instance(instance) != kInvalidMaterialInstanceId);
    CHECK(destination.load_parameter_collection(path, &error));
    CHECK(destination.resolved(44) && std::fabs(destination.resolved(44)->roughness - 0.4F) < 1.0e-6F);
    std::filesystem::remove_all(root);
}

void test_clear_coat_and_foliage_fields_resolve() {
    std::unordered_map<MasterMaterialId, MasterMaterial> masters;
    MasterMaterial coat;
    coat.id = 1; coat.name = "CarPaint"; coat.shadingModel = MaterialShadingModel::ClearCoat;
    coat.parameters.scalars.push_back({std::string(kClearCoatParam), 1.0F, 0.0F, 1.0F});
    coat.parameters.scalars.push_back({std::string(kClearCoatRoughnessParam), 0.08F, 0.0F, 1.0F});
    masters[1] = coat;
    MaterialInstance paint;
    paint.id = 1; paint.name = "RedPaint"; paint.materialId = 1; paint.master = 1;
    auto result = resolve_material_instance(1, masters, {{1, paint}});
    CHECK(result.success());
    CHECK(result.definition.shadingModel == MaterialShadingModel::ClearCoat);
    CHECK(result.definition.clearCoat == 1.0F);
    CHECK(std::fabs(result.definition.clearCoatRoughness - 0.08F) < 1.0e-6F);

    MasterMaterial foliage;
    foliage.id = 2; foliage.name = "Leaf"; foliage.shadingModel = MaterialShadingModel::TwoSidedFoliage;
    foliage.parameters.vectors.push_back({std::string(kFoliageColorParam), {0.2F, 0.8F, 0.1F, 1.0F}});
    foliage.parameters.scalars.push_back({std::string(kFoliageTransmittanceParam), 0.7F, 0.0F, 1.0F});
    foliage.parameters.scalars.push_back({std::string(kFoliageWrapParam), 0.4F, 0.0F, 1.0F});
    masters[2] = foliage;
    MaterialInstance leaf;
    leaf.id = 2; leaf.name = "Leaf01"; leaf.materialId = 2; leaf.master = 2;
    result = resolve_material_instance(2, masters, {{2, leaf}});
    CHECK(result.success());
    CHECK(result.definition.shadingModel == MaterialShadingModel::TwoSidedFoliage);
    CHECK(std::fabs(result.definition.foliageColor.y - 0.8F) < 1.0e-6F);
    CHECK(std::fabs(result.definition.foliageTransmittance - 0.7F) < 1.0e-6F);
}

} // namespace

int main() {
    test_standard_surface_factory();
    test_basic_master_and_instance();
    test_instance_overrides();
    test_instance_of_instance_chain();
    test_cycle_detection();
    test_unknown_master();
    test_unknown_parent();
    test_unknown_instance();
    test_inconsistent_master();
    test_unknown_parameter_rejected();
    test_scalar_out_of_range_rejected();
    test_material_library_basic_flow();
    test_material_library_rejects_duplicate_material_id();
    test_material_library_partial_failure_keeps_good_instances();
    test_runtime_overrides();
    test_runtime_override_validation();
    test_new_shading_fields_resolve_correctly();
    test_material_parameter_collection_bindings_and_rollback();
    test_material_layers_and_runtime_weight();
    test_material_layer_cycle_is_rejected();
    test_material_library_collection_save_load_re_resolves();
    test_clear_coat_and_foliage_fields_resolve();

    if (failures == 0) {
        std::cout << "dve_master_material_tests: PASS\n";
        return 0;
    }
    std::cerr << "dve_master_material_tests: " << failures << " FAILURE(S)\n";
    return 1;
}
