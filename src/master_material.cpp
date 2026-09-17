#include "dve/master_material.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>

namespace dve {
namespace {

template <class T>
const T* find_named(const std::vector<T>& values, std::string_view name) noexcept {
    for (const T& value : values) if (value.name == name) return &value;
    return nullptr;
}

bool finite(Float4 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

float combine_scalar(float local, float global, MaterialGlobalCombine combine) noexcept {
    switch (combine) {
    case MaterialGlobalCombine::Replace: return global;
    case MaterialGlobalCombine::Add: return local + global;
    case MaterialGlobalCombine::Multiply: return local * global;
    }
    return local;
}

Float4 combine_vector(Float4 local, Float4 global, MaterialGlobalCombine combine) noexcept {
    switch (combine) {
    case MaterialGlobalCombine::Replace: return global;
    case MaterialGlobalCombine::Add:
        return {local.x + global.x, local.y + global.y, local.z + global.z, local.w + global.w};
    case MaterialGlobalCombine::Multiply:
        return {local.x * global.x, local.y * global.y, local.z * global.z, local.w * global.w};
    }
    return local;
}

float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
Float3 lerp(Float3 a, Float3 b, float t) noexcept {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}
Float4 lerp(Float4 a, Float4 b, float t) noexcept {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)};
}
float saturate(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }
Float3 add(Float3 a, Float3 b, float weight) noexcept {
    return {std::max(0.0F, a.x + b.x * weight), std::max(0.0F, a.y + b.y * weight),
            std::max(0.0F, a.z + b.z * weight)};
}
Float4 add(Float4 a, Float4 b, float weight) noexcept {
    return {std::max(0.0F, a.x + b.x * weight), std::max(0.0F, a.y + b.y * weight),
            std::max(0.0F, a.z + b.z * weight), std::max(0.0F, a.w + b.w * weight)};
}
Float3 multiply_layer(Float3 a, Float3 b, float weight) noexcept {
    return {a.x * lerp(1.0F, b.x, weight), a.y * lerp(1.0F, b.y, weight),
            a.z * lerp(1.0F, b.z, weight)};
}
Float4 multiply_layer(Float4 a, Float4 b, float weight) noexcept {
    return {a.x * lerp(1.0F, b.x, weight), a.y * lerp(1.0F, b.y, weight),
            a.z * lerp(1.0F, b.z, weight), a.w * lerp(1.0F, b.w, weight)};
}

void map_reserved_parameters(MaterialResolveResult& result, const MaterialInstance& leaf,
                             const MasterMaterial& master) {
    result.definition.name = leaf.name;
    result.definition.shadingModel = master.shadingModel;
    result.definition.blendMode = master.blendMode;
    if (const auto it = result.resolvedVectors.find(std::string(kBaseColorParam)); it != result.resolvedVectors.end())
        result.definition.baseColor = it->second;
    if (const auto it = result.resolvedVectors.find(std::string(kEmissiveParam)); it != result.resolvedVectors.end())
        result.definition.emissive = {it->second.x, it->second.y, it->second.z};
    if (const auto it = result.resolvedScalars.find(std::string(kMetallicParam)); it != result.resolvedScalars.end())
        result.definition.metallic = it->second;
    if (const auto it = result.resolvedScalars.find(std::string(kRoughnessParam)); it != result.resolvedScalars.end())
        result.definition.roughness = it->second;
    if (const auto it = result.resolvedScalars.find(std::string(kSpecularParam)); it != result.resolvedScalars.end())
        result.definition.specular = it->second;
    if (const auto it = result.resolvedSwitches.find(std::string(kTransparentParam)); it != result.resolvedSwitches.end())
        result.definition.transparent = it->second;
    if (const auto it = result.resolvedVectors.find(std::string(kSubsurfaceColorParam)); it != result.resolvedVectors.end())
        result.definition.subsurfaceColor = {it->second.x, it->second.y, it->second.z};
    if (const auto it = result.resolvedScalars.find(std::string(kSubsurfaceScatterDistanceParam)); it != result.resolvedScalars.end())
        result.definition.subsurfaceScatterDistanceMeters = it->second;
    if (const auto it = result.resolvedScalars.find(std::string(kClearCoatParam)); it != result.resolvedScalars.end())
        result.definition.clearCoat = it->second;
    if (const auto it = result.resolvedScalars.find(std::string(kClearCoatRoughnessParam)); it != result.resolvedScalars.end())
        result.definition.clearCoatRoughness = it->second;
    if (const auto it = result.resolvedVectors.find(std::string(kFoliageColorParam)); it != result.resolvedVectors.end())
        result.definition.foliageColor = {it->second.x, it->second.y, it->second.z};
    if (const auto it = result.resolvedScalars.find(std::string(kFoliageTransmittanceParam)); it != result.resolvedScalars.end())
        result.definition.foliageTransmittance = it->second;
    if (const auto it = result.resolvedScalars.find(std::string(kFoliageWrapParam)); it != result.resolvedScalars.end())
        result.definition.foliageWrap = it->second;
    result.definition.layers = leaf.layers;
    result.definition.densityKilogramsPerCubicMeter = leaf.densityKilogramsPerCubicMeter;
    result.definition.structuralStrength = leaf.structuralStrength;
    result.definition.fractureResistance = leaf.fractureResistance;
    result.definition.flammability = leaf.flammability;
    result.definition.thermalConductivity = leaf.thermalConductivity;
    result.definition.structural = leaf.structural;
}

} // namespace

MasterMaterial make_standard_surface_master_material() {
    MasterMaterial master;
    master.name = std::string(kStandardSurfaceMasterName);
    master.shadingModel = MaterialShadingModel::StandardPBR;
    master.blendMode = MaterialBlendMode::Opaque;
    master.parameters.vectors.push_back({std::string(kBaseColorParam), {0.214F, 0.214F, 0.214F, 1.0F}});
    master.parameters.vectors.push_back({std::string(kEmissiveParam), {0.0F, 0.0F, 0.0F, 0.0F}});
    master.parameters.scalars.push_back({std::string(kMetallicParam), 0.0F, 0.0F, 1.0F});
    master.parameters.scalars.push_back({std::string(kRoughnessParam), kStandardSurfaceRoughness, 0.0F, 1.0F});
    master.parameters.scalars.push_back({std::string(kSpecularParam), kStandardSurfaceSpecular, 0.0F, 1.0F});
    master.parameters.switches.push_back({std::string(kTransparentParam), false});
    return master;
}

const MaterialScalarParam* MaterialParameterSchema::find_scalar(std::string_view name) const noexcept {
    return find_named(scalars, name);
}
const MaterialVectorParam* MaterialParameterSchema::find_vector(std::string_view name) const noexcept {
    return find_named(vectors, name);
}
const MaterialSwitchParam* MaterialParameterSchema::find_switch(std::string_view name) const noexcept {
    return find_named(switches, name);
}

MaterialResolveResult resolve_material_instance(
    MaterialInstanceId id,
    const std::unordered_map<MasterMaterialId, MasterMaterial>& masters,
    const std::unordered_map<MaterialInstanceId, MaterialInstance>& instances,
    const MaterialParameterCollection* globals) {
    MaterialResolveResult result;
    const auto leafIt = instances.find(id);
    if (leafIt == instances.end()) {
        result.error = MaterialResolveErrorCode::UnknownInstance;
        result.errorDetail = "unknown material instance";
        return result;
    }

    std::vector<const MaterialInstance*> chain;
    std::unordered_set<MaterialInstanceId> visited;
    const MaterialInstance* current = &leafIt->second;
    while (current != nullptr) {
        if (!visited.insert(current->id).second) {
            result.error = MaterialResolveErrorCode::CyclicInheritance;
            result.errorDetail = "cyclic material-instance inheritance";
            return result;
        }
        chain.push_back(current);
        if (!current->parent) break;
        const auto parentIt = instances.find(*current->parent);
        if (parentIt == instances.end()) {
            result.error = MaterialResolveErrorCode::UnknownParent;
            result.errorDetail = "unknown parent material instance";
            return result;
        }
        current = &parentIt->second;
    }

    MasterMaterialId masterId = kInvalidMasterMaterialId;
    for (const MaterialInstance* instance : chain) {
        if (instance->master == kInvalidMasterMaterialId) continue;
        if (masterId != kInvalidMasterMaterialId && masterId != instance->master) {
            result.error = MaterialResolveErrorCode::InconsistentMaster;
            result.errorDetail = "instance chain names inconsistent master materials";
            return result;
        }
        masterId = instance->master;
    }
    const auto masterIt = masters.find(masterId);
    if (masterId == kInvalidMasterMaterialId || masterIt == masters.end()) {
        result.error = MaterialResolveErrorCode::UnknownMaster;
        result.errorDetail = "unknown or missing master material";
        return result;
    }
    const MasterMaterial& master = masterIt->second;
    result.master = masterId;

    for (const MaterialScalarParam& parameter : master.parameters.scalars) {
        result.resolvedScalars[parameter.name] = parameter.defaultValue;
    }
    for (const MaterialVectorParam& parameter : master.parameters.vectors) {
        result.resolvedVectors[parameter.name] = parameter.defaultValue;
    }
    for (const MaterialSwitchParam& parameter : master.parameters.switches) {
        result.resolvedSwitches[parameter.name] = parameter.defaultValue;
    }

    // The chain was collected leaf-to-root, so apply root-to-leaf.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const MaterialInstance& instance = **it;
        for (const auto& [name, value] : instance.scalarOverrides) {
            const MaterialScalarParam* parameter = master.parameters.find_scalar(name);
            if (!parameter) {
                result.error = MaterialResolveErrorCode::UnknownParameter;
                result.errorDetail = "unknown scalar parameter \"" + name + "\"";
                return result;
            }
            if (!std::isfinite(value) || value < parameter->minValue || value > parameter->maxValue) {
                result.error = MaterialResolveErrorCode::ScalarOutOfRange;
                result.errorDetail = "scalar parameter \"" + name + "\" is outside its declared range";
                return result;
            }
            result.resolvedScalars[name] = value;
        }
        for (const auto& [name, value] : instance.vectorOverrides) {
            if (!master.parameters.find_vector(name)) {
                result.error = MaterialResolveErrorCode::UnknownParameter;
                result.errorDetail = "unknown vector parameter \"" + name + "\"";
                return result;
            }
            if (!finite(value)) {
                result.error = MaterialResolveErrorCode::UnknownParameter;
                result.errorDetail = "vector parameter \"" + name + "\" is non-finite";
                return result;
            }
            result.resolvedVectors[name] = value;
        }
        for (const auto& [name, value] : instance.switchOverrides) {
            if (!master.parameters.find_switch(name)) {
                result.error = MaterialResolveErrorCode::UnknownParameter;
                result.errorDetail = "unknown switch parameter \"" + name + "\"";
                return result;
            }
            result.resolvedSwitches[name] = value;
        }
    }

    if (globals != nullptr) {
        for (const MaterialGlobalScalarBinding& binding : master.globalScalars) {
            const MaterialScalarParam* parameter = master.parameters.find_scalar(binding.parameterName);
            if (!parameter) {
                result.error = MaterialResolveErrorCode::InvalidGlobalBinding;
                result.errorDetail = "global scalar binding targets unknown parameter \"" + binding.parameterName + "\"";
                return result;
            }
            const auto global = globals->scalar(binding.globalName);
            if (!global) continue; // a collection can be populated after the master is authored
            const float combined = combine_scalar(result.resolvedScalars[binding.parameterName], *global, binding.combine);
            if (!std::isfinite(combined) || combined < parameter->minValue || combined > parameter->maxValue) {
                result.error = MaterialResolveErrorCode::GlobalParameterOutOfRange;
                result.errorDetail = "global scalar \"" + binding.globalName + "\" drives \"" +
                                     binding.parameterName + "\" outside its declared range";
                return result;
            }
            result.resolvedScalars[binding.parameterName] = combined;
        }
        for (const MaterialGlobalVectorBinding& binding : master.globalVectors) {
            if (!master.parameters.find_vector(binding.parameterName)) {
                result.error = MaterialResolveErrorCode::InvalidGlobalBinding;
                result.errorDetail = "global vector binding targets unknown parameter \"" + binding.parameterName + "\"";
                return result;
            }
            const auto global = globals->vector(binding.globalName);
            if (!global) continue;
            const Float4 combined = combine_vector(result.resolvedVectors[binding.parameterName], *global, binding.combine);
            if (!finite(combined)) {
                result.error = MaterialResolveErrorCode::InvalidGlobalBinding;
                result.errorDetail = "global vector binding produced a non-finite value";
                return result;
            }
            result.resolvedVectors[binding.parameterName] = combined;
        }
    }

    const MaterialInstance& leaf = leafIt->second;
    if (leaf.layers.size() > kMaximumVoxelMaterialLayers) {
        result.error = MaterialResolveErrorCode::TooManyLayers;
        result.errorDetail = "material layer stack exceeds the fixed four-layer limit";
        return result;
    }
    for (const VoxelMaterialLayer& layer : leaf.layers) {
        if (!std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F) {
            result.error = MaterialResolveErrorCode::InvalidLayerWeight;
            result.errorDetail = "material layer weight must be finite and in [0,1]";
            return result;
        }
        if (static_cast<unsigned>(layer.blendMode) > static_cast<unsigned>(MaterialLayerBlendMode::Additive)) {
            result.error = MaterialResolveErrorCode::InvalidLayerWeight;
            result.errorDetail = "material layer blend mode is invalid";
            return result;
        }
    }

    map_reserved_parameters(result, leaf, master);
    return result;
}

VoxelMaterialDefinition blend_material_layer(const VoxelMaterialDefinition& base,
                                             const VoxelMaterialDefinition& layer,
                                             float weight,
                                             MaterialLayerBlendMode blendMode) noexcept {
    const float t = saturate(weight);
    VoxelMaterialDefinition out = base;
    switch (blendMode) {
    case MaterialLayerBlendMode::Lerp:
        out.baseColor = lerp(base.baseColor, layer.baseColor, t);
        out.emissive = lerp(base.emissive, layer.emissive, t);
        out.metallic = lerp(base.metallic, layer.metallic, t);
        out.roughness = lerp(base.roughness, layer.roughness, t);
        out.specular = lerp(base.specular, layer.specular, t);
        out.subsurfaceScatterDistanceMeters = lerp(base.subsurfaceScatterDistanceMeters,
                                                   layer.subsurfaceScatterDistanceMeters, t);
        out.subsurfaceColor = lerp(base.subsurfaceColor, layer.subsurfaceColor, t);
        out.clearCoat = lerp(base.clearCoat, layer.clearCoat, t);
        out.clearCoatRoughness = lerp(base.clearCoatRoughness, layer.clearCoatRoughness, t);
        out.foliageColor = lerp(base.foliageColor, layer.foliageColor, t);
        out.foliageTransmittance = lerp(base.foliageTransmittance, layer.foliageTransmittance, t);
        out.foliageWrap = lerp(base.foliageWrap, layer.foliageWrap, t);
        break;
    case MaterialLayerBlendMode::Multiply:
        out.baseColor = multiply_layer(base.baseColor, layer.baseColor, t);
        out.emissive = add(base.emissive, layer.emissive, t);
        out.metallic = saturate(lerp(base.metallic, base.metallic * layer.metallic, t));
        out.roughness = saturate(lerp(base.roughness, base.roughness * layer.roughness, t));
        out.specular = saturate(lerp(base.specular, base.specular * layer.specular, t));
        out.subsurfaceColor = multiply_layer(base.subsurfaceColor, layer.subsurfaceColor, t);
        out.clearCoat = saturate(lerp(base.clearCoat, base.clearCoat * layer.clearCoat, t));
        out.clearCoatRoughness = saturate(lerp(base.clearCoatRoughness,
                                              base.clearCoatRoughness * layer.clearCoatRoughness, t));
        out.foliageColor = multiply_layer(base.foliageColor, layer.foliageColor, t);
        out.foliageTransmittance = saturate(lerp(base.foliageTransmittance,
                                                 base.foliageTransmittance * layer.foliageTransmittance, t));
        out.foliageWrap = saturate(lerp(base.foliageWrap, base.foliageWrap * layer.foliageWrap, t));
        break;
    case MaterialLayerBlendMode::Additive:
        out.baseColor = add(base.baseColor, layer.baseColor, t);
        out.emissive = add(base.emissive, layer.emissive, t);
        out.metallic = saturate(base.metallic + layer.metallic * t);
        out.roughness = saturate(base.roughness + layer.roughness * t);
        out.specular = saturate(base.specular + layer.specular * t);
        out.subsurfaceScatterDistanceMeters = std::max(0.0F, base.subsurfaceScatterDistanceMeters +
                                                              layer.subsurfaceScatterDistanceMeters * t);
        out.subsurfaceColor = add(base.subsurfaceColor, layer.subsurfaceColor, t);
        out.clearCoat = saturate(base.clearCoat + layer.clearCoat * t);
        out.clearCoatRoughness = saturate(base.clearCoatRoughness + layer.clearCoatRoughness * t);
        out.foliageColor = add(base.foliageColor, layer.foliageColor, t);
        out.foliageTransmittance = saturate(base.foliageTransmittance + layer.foliageTransmittance * t);
        out.foliageWrap = saturate(base.foliageWrap + layer.foliageWrap * t);
        break;
    }
    // The target layer stack owns these semantic/physical properties.
    out.name = base.name;
    out.shadingModel = base.shadingModel;
    out.blendMode = base.blendMode;
    out.transparent = base.transparent;
    out.structural = base.structural;
    out.densityKilogramsPerCubicMeter = base.densityKilogramsPerCubicMeter;
    out.structuralStrength = base.structuralStrength;
    out.fractureResistance = base.fractureResistance;
    out.flammability = base.flammability;
    out.thermalConductivity = base.thermalConductivity;
    out.layers = base.layers;
    return out;
}

std::optional<std::vector<VoxelMaterialDefinition>> resolve_voxel_material_layers(
    const std::vector<VoxelMaterialDefinition>& materials, std::string* error) {
    if (materials.size() > 256) {
        if (error) *error = "material table exceeds 256 entries";
        return std::nullopt;
    }
    std::vector<VoxelMaterialDefinition> output(materials.size());
    std::vector<std::uint8_t> state(materials.size(), 0);
    std::function<bool(std::size_t)> visit = [&](std::size_t id) {
        if (state[id] == 2) return true;
        if (state[id] == 1) {
            if (error) *error = "cyclic material layer stack";
            return false;
        }
        state[id] = 1;
        VoxelMaterialDefinition composed = materials[id];
        if (composed.layers.size() > kMaximumVoxelMaterialLayers) {
            if (error) *error = "material layer stack exceeds four layers";
            return false;
        }
        for (const VoxelMaterialLayer& layer : composed.layers) {
            if (!layer.enabled || layer.weight <= 0.0F) continue;
            if (!std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F ||
                layer.sourceMaterial >= materials.size()) {
                if (error) *error = "invalid material layer reference or weight";
                return false;
            }
            if (!visit(layer.sourceMaterial)) return false;
            composed = blend_material_layer(composed, output[layer.sourceMaterial], layer.weight, layer.blendMode);
        }
        output[id] = std::move(composed);
        state[id] = 2;
        return true;
    };
    for (std::size_t id = 0; id < materials.size(); ++id) if (!visit(id)) return std::nullopt;
    return output;
}

bool MaterialLibrary::validate_master(const MasterMaterial& master, std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (master.name.empty()) return fail("master material name must not be empty");
    if (static_cast<unsigned>(master.shadingModel) > static_cast<unsigned>(MaterialShadingModel::ClearCoat) ||
        static_cast<unsigned>(master.blendMode) > static_cast<unsigned>(MaterialBlendMode::Translucent))
        return fail("master material shading or blend mode is invalid");
    std::unordered_set<std::string> names;
    for (const MaterialScalarParam& parameter : master.parameters.scalars) {
        if (parameter.name.empty() || !std::isfinite(parameter.defaultValue) || !std::isfinite(parameter.minValue) ||
            !std::isfinite(parameter.maxValue) || parameter.minValue > parameter.maxValue ||
            parameter.defaultValue < parameter.minValue || parameter.defaultValue > parameter.maxValue ||
            !names.insert(parameter.name).second) return fail("invalid or duplicate scalar parameter schema");
    }
    for (const MaterialVectorParam& parameter : master.parameters.vectors) {
        if (parameter.name.empty() || !finite(parameter.defaultValue) || !names.insert(parameter.name).second)
            return fail("invalid or duplicate vector parameter schema");
    }
    for (const MaterialSwitchParam& parameter : master.parameters.switches) {
        if (parameter.name.empty() || !names.insert(parameter.name).second)
            return fail("invalid or duplicate switch parameter schema");
    }
    std::unordered_set<std::string> boundTargets;
    for (const MaterialGlobalScalarBinding& binding : master.globalScalars) {
        if (binding.globalName.empty() || !master.parameters.find_scalar(binding.parameterName) ||
            !boundTargets.insert("s:" + binding.parameterName).second)
            return fail("invalid or duplicate global scalar binding");
    }
    for (const MaterialGlobalVectorBinding& binding : master.globalVectors) {
        if (binding.globalName.empty() || !master.parameters.find_vector(binding.parameterName) ||
            !boundTargets.insert("v:" + binding.parameterName).second)
            return fail("invalid or duplicate global vector binding");
    }
    return true;
}

MasterMaterialId MaterialLibrary::add_master(MasterMaterial master, std::string* error) {
    if (!validate_master(master, error)) return kInvalidMasterMaterialId;
    const MasterMaterialId id = nextMasterId_++;
    master.id = id;
    masters_.emplace(id, std::move(master));
    std::string ignored;
    (void)resolve_all(&ignored);
    return id;
}

bool MaterialLibrary::update_master(const MasterMaterial& master, std::string* error) {
    const auto it = masters_.find(master.id);
    if (it == masters_.end()) { if (error) *error = "unknown master material id"; return false; }
    if (!validate_master(master, error)) return false;
    const MasterMaterial previous = it->second;
    it->second = master;
    std::string resolveError;
    if (!resolve_all(&resolveError)) {
        it->second = previous;
        (void)resolve_all(nullptr);
        if (error) *error = resolveError;
        return false;
    }
    return true;
}

const MasterMaterial* MaterialLibrary::find_master(MasterMaterialId id) const noexcept {
    const auto it = masters_.find(id);
    return it == masters_.end() ? nullptr : &it->second;
}

std::optional<MasterMaterialId> MaterialLibrary::find_master_by_name(std::string_view name) const {
    for (const auto& [id, master] : masters_) if (master.name == name) return id;
    return std::nullopt;
}

MaterialInstanceId MaterialLibrary::add_instance(MaterialInstance instance, std::string* error) {
    if (instance.name.empty()) { if (error) *error = "material instance name must not be empty"; return kInvalidMaterialInstanceId; }
    if (instance.layers.size() > kMaximumVoxelMaterialLayers) {
        if (error) *error = "material instance exceeds four layers";
        return kInvalidMaterialInstanceId;
    }
    if (const auto existing = find_instance_by_material_id(instance.materialId)) {
        if (error) *error = "material id " + std::to_string(instance.materialId) + " is already used by instance " + std::to_string(*existing);
        return kInvalidMaterialInstanceId;
    }
    const MaterialInstanceId id = nextInstanceId_++;
    instance.id = id;
    instances_.emplace(id, std::move(instance));
    std::string resolveError;
    (void)resolve_all(&resolveError); // partial failure is retained for editor repair workflows
    if (!resolveError.empty() && error) *error = resolveError;
    return id;
}

bool MaterialLibrary::update_instance(const MaterialInstance& instance, std::string* error) {
    const auto it = instances_.find(instance.id);
    if (it == instances_.end()) { if (error) *error = "unknown material instance id"; return false; }
    if (instance.layers.size() > kMaximumVoxelMaterialLayers) { if (error) *error = "material instance exceeds four layers"; return false; }
    if (instance.materialId != it->second.materialId) {
        if (const auto existing = find_instance_by_material_id(instance.materialId); existing && *existing != instance.id) {
            if (error) *error = "material id is already used by another instance";
            return false;
        }
    }
    it->second = instance;
    std::string resolveError;
    const bool ok = resolve_all(&resolveError);
    if (!ok && error) *error = resolveError;
    return true;
}

const MaterialInstance* MaterialLibrary::find_instance(MaterialInstanceId id) const noexcept {
    const auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : &it->second;
}

std::optional<MaterialInstanceId> MaterialLibrary::find_instance_by_material_id(MaterialId materialId) const {
    const auto it = materialIdToInstance_.find(materialId);
    return it == materialIdToInstance_.end() ? std::nullopt : std::optional<MaterialInstanceId>(it->second);
}

void MaterialLibrary::apply_runtime_overrides(MaterialId materialId, VoxelMaterialDefinition& definition) const {
    if (const auto it = runtimeScalars_.find(materialId); it != runtimeScalars_.end()) {
        for (const auto& [name, value] : it->second) {
            if (name == kMetallicParam) definition.metallic = value;
            else if (name == kRoughnessParam) definition.roughness = value;
            else if (name == kSpecularParam) definition.specular = value;
            else if (name == kSubsurfaceScatterDistanceParam) definition.subsurfaceScatterDistanceMeters = value;
            else if (name == kClearCoatParam) definition.clearCoat = value;
            else if (name == kClearCoatRoughnessParam) definition.clearCoatRoughness = value;
            else if (name == kFoliageTransmittanceParam) definition.foliageTransmittance = value;
            else if (name == kFoliageWrapParam) definition.foliageWrap = value;
        }
    }
    if (const auto it = runtimeVectors_.find(materialId); it != runtimeVectors_.end()) {
        for (const auto& [name, value] : it->second) {
            if (name == kBaseColorParam) definition.baseColor = value;
            else if (name == kEmissiveParam) definition.emissive = {value.x, value.y, value.z};
            else if (name == kSubsurfaceColorParam) definition.subsurfaceColor = {value.x, value.y, value.z};
            else if (name == kFoliageColorParam) definition.foliageColor = {value.x, value.y, value.z};
        }
    }
}

bool MaterialLibrary::resolve_all(std::string* error) {
    results_.clear();
    materialIdToInstance_.clear();
    resolvedByMaterialId_.clear();
    bool allSucceeded = true;
    std::string firstError;
    std::unordered_map<MaterialId, VoxelMaterialDefinition> bases;

    for (const auto& [id, instance] : instances_) {
        MaterialResolveResult result = resolve_material_instance(id, masters_, instances_, &globals_);
        if (result.success()) {
            materialIdToInstance_[instance.materialId] = id;
            VoxelMaterialDefinition definition = result.definition;
            apply_runtime_overrides(instance.materialId, definition);
            if (const auto weights = runtimeLayerWeights_.find(instance.materialId); weights != runtimeLayerWeights_.end()) {
                for (const auto& [index, weight] : weights->second) if (index < definition.layers.size()) definition.layers[index].weight = weight;
            }
            bases[instance.materialId] = definition;
            result.definition = definition;
        } else if (allSucceeded) {
            allSucceeded = false;
            firstError = "instance " + std::to_string(id) + " (\"" + instance.name + "\"): " + result.errorDetail;
        }
        results_[id] = std::move(result);
    }

    std::array<std::uint8_t, 256> state{};
    std::function<bool(MaterialId)> compose = [&](MaterialId materialId) {
        if (state[materialId] == 2) return true;
        if (state[materialId] == 1) {
            const auto target = materialIdToInstance_.find(materialId);
            if (target != materialIdToInstance_.end()) {
                auto& result = results_[target->second];
                result.error = MaterialResolveErrorCode::CyclicLayerStack;
                result.errorDetail = "cyclic material layer stack";
            }
            return false;
        }
        const auto baseIt = bases.find(materialId);
        if (baseIt == bases.end()) return false;
        state[materialId] = 1;
        VoxelMaterialDefinition composed = baseIt->second;
        for (const VoxelMaterialLayer& layer : composed.layers) {
            if (!layer.enabled || layer.weight <= 0.0F) continue;
            if (!std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F) {
                const auto target = materialIdToInstance_.find(materialId);
                if (target != materialIdToInstance_.end()) {
                    auto& result = results_[target->second];
                    result.error = MaterialResolveErrorCode::InvalidLayerWeight;
                    result.errorDetail = "material layer weight must be finite and in [0,1]";
                }
                return false;
            }
            if (!bases.contains(layer.sourceMaterial)) {
                const auto target = materialIdToInstance_.find(materialId);
                if (target != materialIdToInstance_.end()) {
                    auto& result = results_[target->second];
                    result.error = MaterialResolveErrorCode::UnknownLayerMaterial;
                    result.errorDetail = "material layer references unresolved material id " + std::to_string(layer.sourceMaterial);
                }
                return false;
            }
            if (!compose(layer.sourceMaterial)) return false;
            composed = blend_material_layer(composed, resolvedByMaterialId_.at(layer.sourceMaterial), layer.weight, layer.blendMode);
        }
        resolvedByMaterialId_[materialId] = composed;
        if (const auto target = materialIdToInstance_.find(materialId); target != materialIdToInstance_.end()) {
            results_[target->second].definition = composed;
        }
        state[materialId] = 2;
        return true;
    };

    for (const auto& [materialId, instanceId] : materialIdToInstance_) {
        if (!compose(materialId)) {
            if (allSucceeded) {
                allSucceeded = false;
                firstError = "instance " + std::to_string(instanceId) + ": " + results_[instanceId].errorDetail;
            }
            resolvedByMaterialId_.erase(materialId);
        }
    }
    if (!allSucceeded && error) *error = firstError;
    return allSucceeded;
}

const MaterialResolveResult* MaterialLibrary::last_result(MaterialInstanceId id) const {
    const auto it = results_.find(id);
    return it == results_.end() ? nullptr : &it->second;
}

const VoxelMaterialDefinition* MaterialLibrary::resolved(MaterialId materialId) const noexcept {
    const auto it = resolvedByMaterialId_.find(materialId);
    return it == resolvedByMaterialId_.end() ? nullptr : &it->second;
}

bool MaterialLibrary::set_runtime_scalar(MaterialId materialId, std::string_view name, float value, std::string* error) {
    const auto instanceId = find_instance_by_material_id(materialId);
    if (!instanceId) { if (error) *error = "no material instance uses material id " + std::to_string(materialId); return false; }
    const MaterialResolveResult* result = last_result(*instanceId);
    const MasterMaterial* master = result ? find_master(result->master) : nullptr;
    const MaterialScalarParam* parameter = master ? master->parameters.find_scalar(name) : nullptr;
    if (!parameter) { if (error) *error = "unknown scalar parameter \"" + std::string(name) + "\""; return false; }
    if (!std::isfinite(value) || value < parameter->minValue || value > parameter->maxValue) {
        if (error) *error = "value out of range";
        return false;
    }
    const auto previous = runtimeScalars_[materialId];
    runtimeScalars_[materialId][std::string(name)] = value;
    std::string resolveError;
    (void)resolve_all(&resolveError);
    if (!resolved(materialId)) {
        runtimeScalars_[materialId] = previous;
        (void)resolve_all(nullptr);
        if (error) *error = resolveError.empty() ? "runtime material update failed" : resolveError;
        return false;
    }
    if (error) error->clear();
    return true;
}

bool MaterialLibrary::set_runtime_vector(MaterialId materialId, std::string_view name, Float4 value, std::string* error) {
    const auto instanceId = find_instance_by_material_id(materialId);
    if (!instanceId) { if (error) *error = "no material instance uses material id " + std::to_string(materialId); return false; }
    const MaterialResolveResult* result = last_result(*instanceId);
    const MasterMaterial* master = result ? find_master(result->master) : nullptr;
    if (!master || !master->parameters.find_vector(name)) { if (error) *error = "unknown vector parameter \"" + std::string(name) + "\""; return false; }
    if (!finite(value)) { if (error) *error = "vector value must be finite"; return false; }
    const auto previous = runtimeVectors_[materialId];
    runtimeVectors_[materialId][std::string(name)] = value;
    std::string resolveError;
    (void)resolve_all(&resolveError);
    if (!resolved(materialId)) {
        runtimeVectors_[materialId] = previous;
        (void)resolve_all(nullptr);
        if (error) *error = resolveError.empty() ? "runtime material update failed" : resolveError;
        return false;
    }
    if (error) error->clear();
    return true;
}

std::optional<float> MaterialLibrary::runtime_scalar(MaterialId materialId, std::string_view name) const {
    const auto it = runtimeScalars_.find(materialId);
    if (it == runtimeScalars_.end()) return std::nullopt;
    const auto value = it->second.find(std::string(name));
    return value == it->second.end() ? std::nullopt : std::optional<float>(value->second);
}

std::optional<Float4> MaterialLibrary::runtime_vector(MaterialId materialId, std::string_view name) const {
    const auto it = runtimeVectors_.find(materialId);
    if (it == runtimeVectors_.end()) return std::nullopt;
    const auto value = it->second.find(std::string(name));
    return value == it->second.end() ? std::nullopt : std::optional<Float4>(value->second);
}

bool MaterialLibrary::clear_runtime_overrides(MaterialId materialId) {
    const bool changed = runtimeScalars_.erase(materialId) > 0 || runtimeVectors_.erase(materialId) > 0;
    if (changed) (void)resolve_all(nullptr);
    return changed;
}

bool MaterialLibrary::set_runtime_layer_weight(MaterialId materialId, std::size_t layerIndex, float weight, std::string* error) {
    const auto instanceId = find_instance_by_material_id(materialId);
    const MaterialInstance* instance = instanceId ? find_instance(*instanceId) : nullptr;
    if (!instance || layerIndex >= instance->layers.size()) { if (error) *error = "unknown material layer"; return false; }
    if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) { if (error) *error = "layer weight must be in [0,1]"; return false; }
    const auto previous = runtimeLayerWeights_[materialId];
    runtimeLayerWeights_[materialId][layerIndex] = weight;
    std::string resolveError;
    (void)resolve_all(&resolveError);
    if (!resolved(materialId)) {
        runtimeLayerWeights_[materialId] = previous;
        (void)resolve_all(nullptr);
        if (error) *error = resolveError.empty() ? "runtime layer update failed" : resolveError;
        return false;
    }
    if (error) error->clear();
    return true;
}

std::optional<float> MaterialLibrary::runtime_layer_weight(MaterialId materialId, std::size_t layerIndex) const {
    if (const auto material = runtimeLayerWeights_.find(materialId); material != runtimeLayerWeights_.end()) {
        if (const auto value = material->second.find(layerIndex); value != material->second.end()) return value->second;
    }
    const auto instanceId = find_instance_by_material_id(materialId);
    const MaterialInstance* instance = instanceId ? find_instance(*instanceId) : nullptr;
    return instance && layerIndex < instance->layers.size() ? std::optional<float>(instance->layers[layerIndex].weight) : std::nullopt;
}

bool MaterialLibrary::clear_runtime_layer_weights(MaterialId materialId) {
    const bool changed = runtimeLayerWeights_.erase(materialId) > 0;
    if (changed) (void)resolve_all(nullptr);
    return changed;
}

const std::vector<VoxelMaterialLayer>* MaterialLibrary::layers(MaterialId materialId) const noexcept {
    const auto instanceId = find_instance_by_material_id(materialId);
    const MaterialInstance* instance = instanceId ? find_instance(*instanceId) : nullptr;
    return instance ? &instance->layers : nullptr;
}

bool MaterialLibrary::save_parameter_collection(const std::filesystem::path& path, std::string* error) const {
    return globals_.save(path, error);
}

bool MaterialLibrary::load_parameter_collection(const std::filesystem::path& path, std::string* error) {
    auto loaded = MaterialParameterCollection::load(path, error);
    if (!loaded) return false;
    std::unordered_set<MaterialInstanceId> previouslyResolved;
    for (const auto& [id, result] : results_) if (result.success()) previouslyResolved.insert(id);
    MaterialParameterCollection previous = globals_;
    globals_ = std::move(*loaded);
    std::string resolveError;
    const bool allResolved = resolve_all(&resolveError);
    bool brokePreviouslyResolved = false;
    for (const MaterialInstanceId id : previouslyResolved) {
        const auto it = results_.find(id);
        if (it == results_.end() || !it->second.success()) { brokePreviouslyResolved = true; break; }
    }
    if (brokePreviouslyResolved) {
        globals_ = std::move(previous);
        (void)resolve_all(nullptr);
        if (error) *error = resolveError;
        return false;
    }
    // Pre-existing broken editor instances do not block a valid global update for every material
    // that was already resolvable. Their diagnostics remain available through last_result().
    if (!allResolved && error) error->clear();
    return true;
}

bool MaterialLibrary::uses_global_scalar(std::string_view name) const noexcept {
    for (const auto& [id, master] : masters_) {
        (void)id;
        for (const MaterialGlobalScalarBinding& binding : master.globalScalars) {
            if (binding.globalName == name) return true;
        }
    }
    return false;
}

bool MaterialLibrary::uses_global_vector(std::string_view name) const noexcept {
    for (const auto& [id, master] : masters_) {
        (void)id;
        for (const MaterialGlobalVectorBinding& binding : master.globalVectors) {
            if (binding.globalName == name) return true;
        }
    }
    return false;
}

bool MaterialLibrary::set_global_scalar(std::string_view name, float value, std::string* error) {
    std::unordered_set<MaterialInstanceId> previouslyResolved;
    for (const auto& [id, result] : results_) if (result.success()) previouslyResolved.insert(id);
    MaterialParameterCollection previous = globals_;
    if (!globals_.set_scalar(name, value, error)) return false;
    std::string resolveError;
    const bool allResolved = resolve_all(&resolveError);
    bool brokePreviouslyResolved = false;
    for (const MaterialInstanceId id : previouslyResolved) {
        const auto it = results_.find(id);
        if (it == results_.end() || !it->second.success()) { brokePreviouslyResolved = true; break; }
    }
    if (brokePreviouslyResolved) {
        globals_ = std::move(previous);
        (void)resolve_all(nullptr);
        if (error) *error = resolveError;
        return false;
    }
    if (!allResolved && error) error->clear();
    return true;
}

bool MaterialLibrary::set_global_vector(std::string_view name, Float4 value, std::string* error) {
    std::unordered_set<MaterialInstanceId> previouslyResolved;
    for (const auto& [id, result] : results_) if (result.success()) previouslyResolved.insert(id);
    MaterialParameterCollection previous = globals_;
    if (!globals_.set_vector(name, value, error)) return false;
    std::string resolveError;
    const bool allResolved = resolve_all(&resolveError);
    bool brokePreviouslyResolved = false;
    for (const MaterialInstanceId id : previouslyResolved) {
        const auto it = results_.find(id);
        if (it == results_.end() || !it->second.success()) { brokePreviouslyResolved = true; break; }
    }
    if (brokePreviouslyResolved) {
        globals_ = std::move(previous);
        (void)resolve_all(nullptr);
        if (error) *error = resolveError;
        return false;
    }
    if (!allResolved && error) error->clear();
    return true;
}

} // namespace dve
