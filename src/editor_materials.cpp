#include "dve/editor_materials.hpp"
#include "dve/master_material.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dve::editor {
namespace {

bool finite(float value) noexcept { return std::isfinite(value); }
bool finite(Float3 value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z); }
bool finite(Float4 value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }

VoxelMaterialDefinition make_material(
    std::string name,
    Float4 color,
    float density,
    float strength,
    float fracture,
    float roughness,
    float metallic,
    bool transparent = false,
    bool structural = true) {
    VoxelMaterialDefinition value;
    value.name = std::move(name);
    value.baseColor = color;
    value.densityKilogramsPerCubicMeter = density;
    value.structuralStrength = strength;
    value.fractureResistance = fracture;
    value.roughness = roughness;
    value.metallic = metallic;
    value.transparent = transparent;
    value.blendMode = transparent ? MaterialBlendMode::Translucent : MaterialBlendMode::Opaque;
    value.structural = structural;
    return value;
}

bool validate_definition(const VoxelMaterialDefinition& value, MaterialId id, std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (value.name.empty()) return fail("material name is empty");
    if (!finite(value.baseColor) || !finite(value.emissive)) return fail("material color is non-finite");
    if (!finite(value.metallic) || value.metallic < 0.0F || value.metallic > 1.0F) return fail("metallic must be in [0,1]");
    if (!finite(value.roughness) || value.roughness < 0.0F || value.roughness > 1.0F) return fail("roughness must be in [0,1]");
    if (!finite(value.specular) || value.specular < 0.0F || value.specular > 1.0F) return fail("specular must be in [0,1]");
    if (!finite(value.subsurfaceScatterDistanceMeters) || value.subsurfaceScatterDistanceMeters < 0.0F)
        return fail("subsurface scatter distance must be finite and non-negative");
    if (!finite(value.subsurfaceColor) || value.subsurfaceColor.x < 0.0F || value.subsurfaceColor.y < 0.0F ||
        value.subsurfaceColor.z < 0.0F) return fail("subsurface color must be finite and non-negative");
    if (!finite(value.clearCoat) || value.clearCoat < 0.0F || value.clearCoat > 1.0F ||
        !finite(value.clearCoatRoughness) || value.clearCoatRoughness < 0.0F || value.clearCoatRoughness > 1.0F)
        return fail("clear-coat values must be finite and in [0,1]");
    if (!finite(value.foliageColor) || value.foliageColor.x < 0.0F || value.foliageColor.y < 0.0F ||
        value.foliageColor.z < 0.0F || !finite(value.foliageTransmittance) || value.foliageTransmittance < 0.0F ||
        value.foliageTransmittance > 1.0F || !finite(value.foliageWrap) || value.foliageWrap < 0.0F || value.foliageWrap > 1.0F)
        return fail("foliage values must be finite, non-negative, and normalized where required");
    if (value.layers.size() > kMaximumVoxelMaterialLayers) return fail("material exceeds four layers");
    for (const VoxelMaterialLayer& layer : value.layers) {
        if (!finite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F ||
            static_cast<unsigned>(layer.blendMode) > static_cast<unsigned>(MaterialLayerBlendMode::Additive))
            return fail("material layer is invalid");
    }
    if (static_cast<unsigned>(value.shadingModel) > static_cast<unsigned>(MaterialShadingModel::ClearCoat) ||
        static_cast<unsigned>(value.blendMode) > static_cast<unsigned>(MaterialBlendMode::Translucent))
        return fail("material shading or blend mode is invalid");
    if (!finite(value.densityKilogramsPerCubicMeter) || value.densityKilogramsPerCubicMeter < 0.0F)
        return fail("density must be finite and non-negative");
    if (!finite(value.structuralStrength) || value.structuralStrength < 0.0F ||
        !finite(value.fractureResistance) || value.fractureResistance < 0.0F ||
        !finite(value.flammability) || value.flammability < 0.0F ||
        !finite(value.thermalConductivity) || value.thermalConductivity < 0.0F)
        return fail("physical material values must be finite and non-negative");
    if (id == kAirMaterial && (value.densityKilogramsPerCubicMeter != 0.0F || value.structural || !value.transparent))
        return fail("air must be transparent, non-structural, and zero density");
    return true;
}

} // namespace

EditorMaterialLibrary::EditorMaterialLibrary() : EditorMaterialLibrary(true) {}
EditorMaterialLibrary::EditorMaterialLibrary(bool populateDefaults) {
    if (!populateDefaults) return;
    *this = make_default();
}

EditorMaterialLibrary EditorMaterialLibrary::make_default() {
    EditorMaterialLibrary result(false);
    auto add = [&](MaterialId id, VoxelMaterialDefinition definition) {
        result.entries_.emplace(id, EditorMaterialEntry{id, std::move(definition), true});
    };
    add(0, make_material("Air", {0,0,0,0}, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, true, false));
    add(1, make_material("Concrete", {0.54F,0.56F,0.58F,1}, 2400.0F, 7.0F, 8.0F, 0.88F, 0.0F));
    add(2, make_material("Brick", {0.58F,0.20F,0.12F,1}, 1900.0F, 5.0F, 5.0F, 0.82F, 0.0F));
    add(3, make_material("Wood", {0.52F,0.30F,0.12F,1}, 650.0F, 3.0F, 2.2F, 0.70F, 0.0F));
    result.entries_[3].definition.flammability = 0.8F;
    add(4, make_material("Steel", {0.38F,0.43F,0.48F,1}, 7850.0F, 14.0F, 12.0F, 0.35F, 0.9F));
    add(5, make_material("Glass", {0.34F,0.70F,0.82F,0.38F}, 2500.0F, 1.2F, 0.6F, 0.08F, 0.0F, true, false));
    add(6, make_material("Plaster", {0.82F,0.81F,0.76F,1}, 900.0F, 1.1F, 1.0F, 0.92F, 0.0F));
    add(7, make_material("Soil", {0.28F,0.19F,0.10F,1}, 1450.0F, 0.8F, 0.7F, 1.0F, 0.0F, false, false));
    add(8, make_material("Rubber", {0.05F,0.06F,0.07F,1}, 1100.0F, 1.5F, 3.0F, 0.96F, 0.0F, false, false));
    // Neutral low-reflectivity starter material. 0.214 linear is approximately 0.5 sRGB;
    // 0.25 specular is about 2% dielectric F0 and roughness 0.62 broadens highlights.
    add(kDefaultSurfaceMaterial, make_material(
        "Standard Surface", {0.214F,0.214F,0.214F,1.0F},
        1000.0F, 1.0F, 1.0F, kStandardSurfaceRoughness, 0.0F));
    result.entries_[kDefaultSurfaceMaterial].definition.specular = kStandardSurfaceSpecular;
    return result;
}

const EditorMaterialEntry* EditorMaterialLibrary::find(MaterialId id) const noexcept {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}
EditorMaterialEntry* EditorMaterialLibrary::find(MaterialId id) noexcept {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}
std::vector<EditorMaterialEntry> EditorMaterialLibrary::entries() const {
    std::vector<EditorMaterialEntry> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        (void)id;
        result.push_back(entry);
    }
    return result;
}

bool EditorMaterialLibrary::upsert(EditorMaterialEntry entry, std::string* error) {
    if (!validate_definition(entry.definition, entry.id, error)) return false;
    const auto existing = entries_.find(entry.id);
    if (existing != entries_.end() && existing->second.builtIn && entry.id == kAirMaterial) {
        if (error) *error = "the built-in air material cannot be replaced";
        return false;
    }
    entries_[entry.id] = std::move(entry);
    return true;
}

bool EditorMaterialLibrary::remove(MaterialId id, std::string* error) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (id == kAirMaterial || it->second.builtIn) {
        if (error) *error = "built-in materials cannot be removed";
        return false;
    }
    entries_.erase(it);
    return true;
}

MaterialId EditorMaterialLibrary::next_available_id() const noexcept {
    for (unsigned id = 1; id <= std::numeric_limits<MaterialId>::max(); ++id) {
        const MaterialId candidate = static_cast<MaterialId>(id);
        if (!entries_.contains(candidate)) return candidate;
    }
    return kAirMaterial;
}

bool EditorMaterialLibrary::validate(std::string* error) const {
    if (!entries_.contains(kAirMaterial)) {
        if (error) *error = "material library is missing air";
        return false;
    }
    for (const auto& [id, entry] : entries_) {
        if (entry.id != id || !validate_definition(entry.definition, id, error)) return false;
        for (const VoxelMaterialLayer& layer : entry.definition.layers) {
            if (!entries_.contains(layer.sourceMaterial)) {
                if (error) *error = "material layer references a missing material";
                return false;
            }
        }
    }
    std::vector<VoxelMaterialDefinition> indexed(256);
    std::size_t highest = 0;
    for (const auto& [id, entry] : entries_) { indexed[id] = entry.definition; highest = std::max(highest, static_cast<std::size_t>(id)); }
    indexed.resize(highest + 1);
    std::string layerError;
    if (!resolve_voxel_material_layers(indexed, &layerError)) {
        if (error) *error = layerError;
        return false;
    }
    return true;
}

bool EditorMaterialLibrary::save(const std::filesystem::path& path, std::string* error) const {
    try {
        std::string validation;
        if (!validate(&validation)) throw std::runtime_error(validation);
        std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());
        const std::filesystem::path temporary = path.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not open material library for writing");
        output << "DVE_EDITOR_MATERIALS 3\n" << entries_.size() << "\n" << std::setprecision(9);
        for (const auto& [id, entry] : entries_) {
            const auto& m = entry.definition;
            output << static_cast<unsigned>(id) << ' ' << std::quoted(m.name) << ' '
                   << m.baseColor.x << ' ' << m.baseColor.y << ' ' << m.baseColor.z << ' ' << m.baseColor.w << ' '
                   << m.emissive.x << ' ' << m.emissive.y << ' ' << m.emissive.z << ' '
                   << m.metallic << ' ' << m.roughness << ' ' << m.specular << ' '
                   << static_cast<unsigned>(m.shadingModel) << ' ' << static_cast<unsigned>(m.blendMode) << ' '
                   << m.subsurfaceScatterDistanceMeters << ' '
                   << m.subsurfaceColor.x << ' ' << m.subsurfaceColor.y << ' ' << m.subsurfaceColor.z << ' '
                   << m.clearCoat << ' ' << m.clearCoatRoughness << ' '
                   << m.foliageColor.x << ' ' << m.foliageColor.y << ' ' << m.foliageColor.z << ' '
                   << m.foliageTransmittance << ' ' << m.foliageWrap << ' '
                   << m.densityKilogramsPerCubicMeter << ' ' << m.structuralStrength << ' '
                   << m.fractureResistance << ' ' << m.flammability << ' ' << m.thermalConductivity << ' '
                   << m.transparent << ' ' << m.structural << ' ' << entry.builtIn << ' '
                   << m.layers.size();
            for (const VoxelMaterialLayer& layer : m.layers) {
                output << ' ' << static_cast<unsigned>(layer.sourceMaterial) << ' ' << layer.weight << ' '
                       << static_cast<unsigned>(layer.blendMode) << ' ' << layer.enabled;
            }
            output << '\n';
        }
        output.close();
        if (!output) throw std::runtime_error("failed to write material library");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temporary, path);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

std::optional<EditorMaterialLibrary> EditorMaterialLibrary::load(const std::filesystem::path& path, std::string* error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("could not open material library");
        std::string magic;
        unsigned version{};
        std::size_t count{};
        input >> magic >> version >> count;
        if (magic != "DVE_EDITOR_MATERIALS" || (version != 1 && version != 2 && version != 3) || count > 256)
            throw std::runtime_error("unsupported material library");
        EditorMaterialLibrary result(false);
        for (std::size_t index = 0; index < count; ++index) {
            unsigned rawId{};
            EditorMaterialEntry entry;
            auto& m = entry.definition;
            input >> rawId >> std::quoted(m.name)
                  >> m.baseColor.x >> m.baseColor.y >> m.baseColor.z >> m.baseColor.w
                  >> m.emissive.x >> m.emissive.y >> m.emissive.z
                  >> m.metallic >> m.roughness;
            if (version >= 2) {
                unsigned shadingModel = 0;
                unsigned blendMode = 0;
                input >> m.specular >> shadingModel >> blendMode
                      >> m.subsurfaceScatterDistanceMeters
                      >> m.subsurfaceColor.x >> m.subsurfaceColor.y >> m.subsurfaceColor.z;
                if (shadingModel > static_cast<unsigned>(MaterialShadingModel::ClearCoat) ||
                    blendMode > static_cast<unsigned>(MaterialBlendMode::Translucent))
                    throw std::runtime_error("invalid material shading mode");
                m.shadingModel = static_cast<MaterialShadingModel>(shadingModel);
                m.blendMode = static_cast<MaterialBlendMode>(blendMode);
            }
            if (version >= 3) {
                input >> m.clearCoat >> m.clearCoatRoughness
                      >> m.foliageColor.x >> m.foliageColor.y >> m.foliageColor.z
                      >> m.foliageTransmittance >> m.foliageWrap;
            }
            input >> m.densityKilogramsPerCubicMeter
                  >> m.structuralStrength >> m.fractureResistance >> m.flammability
                  >> m.thermalConductivity >> m.transparent >> m.structural >> entry.builtIn;
            if (version >= 3) {
                std::size_t layerCount{};
                input >> layerCount;
                if (layerCount > kMaximumVoxelMaterialLayers) throw std::runtime_error("material has too many layers");
                m.layers.reserve(layerCount);
                for (std::size_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
                    unsigned source{}; unsigned blend{}; bool enabled{}; float weight{};
                    input >> source >> weight >> blend >> enabled;
                    if (source > 255 || blend > static_cast<unsigned>(MaterialLayerBlendMode::Additive))
                        throw std::runtime_error("invalid material layer record");
                    m.layers.push_back({static_cast<MaterialId>(source), weight,
                                        static_cast<MaterialLayerBlendMode>(blend), enabled});
                }
            }
            if (version == 1 && m.transparent) m.blendMode = MaterialBlendMode::Translucent;
            if (!input || rawId > 255) throw std::runtime_error("malformed material record");
            entry.id = static_cast<MaterialId>(rawId);
            if (result.entries_.contains(entry.id)) throw std::runtime_error("duplicate material identifier");
            result.entries_.emplace(entry.id, std::move(entry));
        }
        std::string validation;
        if (!result.validate(&validation)) throw std::runtime_error(validation);
        return result;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

bool sync_resolved_materials(const dve::MaterialLibrary& source, EditorMaterialLibrary& destination,
                             bool replaceBuiltIns, std::string* error) {
    for (unsigned rawId = 1; rawId <= std::numeric_limits<MaterialId>::max(); ++rawId) {
        const MaterialId id = static_cast<MaterialId>(rawId);
        const VoxelMaterialDefinition* resolved = source.resolved(id);
        if (!resolved) continue;
        if (const EditorMaterialEntry* existing = destination.find(id); existing && existing->builtIn && !replaceBuiltIns)
            continue;
        EditorMaterialEntry entry;
        entry.id = id;
        entry.definition = *resolved;
        entry.builtIn = false;
        if (!destination.upsert(std::move(entry), error)) return false;
    }
    return true;
}

std::string_view editor_material_shading_label(MaterialShadingModel model) noexcept {
    switch (model) {
        case MaterialShadingModel::StandardPBR: return "PBR";
        case MaterialShadingModel::Unlit: return "UNLIT";
        case MaterialShadingModel::Emissive: return "EMIS";
        case MaterialShadingModel::Subsurface: return "SSS";
        case MaterialShadingModel::TwoSidedFoliage: return "FOL";
        case MaterialShadingModel::ClearCoat: return "COAT";
    }
    return "?";
}

std::string editor_material_badge(const VoxelMaterialDefinition& definition) {
    std::string result(editor_material_shading_label(definition.shadingModel));
    if (definition.shadingModel == MaterialShadingModel::StandardPBR && definition.metallic < 0.5F) {
        if (definition.specular <= 0.3F) result += " REFL LOW";
        else if (definition.specular >= 0.7F) result += " REFL HIGH";
    }
    switch (definition.blendMode) {
        case MaterialBlendMode::Opaque: break;
        case MaterialBlendMode::Masked: result += " MASK"; break;
        case MaterialBlendMode::Translucent: result += " BLEND"; break;
    }
    return result;
}

std::vector<std::string> editor_material_warnings(const VoxelMaterialDefinition& definition) {
    std::vector<std::string> warnings;
    if (definition.transparent && definition.blendMode == MaterialBlendMode::Opaque)
        warnings.emplace_back("transparent flag is set while blend mode is opaque");
    if (!definition.transparent && definition.blendMode == MaterialBlendMode::Translucent)
        warnings.emplace_back("translucent blend mode is set while transparent flag is disabled");
    const float emissiveEnergy = definition.emissive.x + definition.emissive.y + definition.emissive.z;
    if (definition.shadingModel == MaterialShadingModel::Emissive && emissiveEnergy <= 1.0e-6F)
        warnings.emplace_back("emissive shading model has no emissive output");
    if (definition.shadingModel == MaterialShadingModel::Subsurface &&
        definition.subsurfaceScatterDistanceMeters <= 1.0e-6F)
        warnings.emplace_back("subsurface shading model has zero scatter distance");
    if (definition.shadingModel == MaterialShadingModel::TwoSidedFoliage &&
        definition.foliageTransmittance <= 1.0e-6F)
        warnings.emplace_back("foliage shading model has no transmission");
    if (definition.shadingModel == MaterialShadingModel::ClearCoat && definition.clearCoat <= 1.0e-6F)
        warnings.emplace_back("clear-coat shading model has zero clear-coat weight");
    return warnings;
}

Float4 editor_material_display_color(const EditorMaterialLibrary& library, MaterialId id) noexcept {
    if (const auto* entry = library.find(id)) return entry->definition.baseColor;
    const float t = static_cast<float>(id) / 255.0F;
    return {0.25F + 0.55F * t, 0.62F - 0.3F * t, 0.85F - 0.5F * t, 1.0F};
}

} // namespace dve::editor
