#include "dve/editor_scene_export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <system_error>

#include "dve/asset_cooker.hpp"
#include "dve/component.hpp"
#include "dve/dvox.hpp"
#include "dve/gabor_volume.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/runtime_scene.hpp"
#include "dve/text3d.hpp"

namespace dve::editor {
namespace {

constexpr std::string_view kManifestSuffix = ".dvoxscene.json";

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2U);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    out += hex.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Rigid transform -> column-major 4x4, the DVOXSCENE worldMatrix convention (same formula as
// the runtime scene checkpoint writer). The quaternion is normalized first so the basis passes
// the loader's orthonormality check.
[[nodiscard]] std::array<float, 16> world_matrix(const RigidTransform& transform) noexcept {
    double x = transform.rotation.x, y = transform.rotation.y, z = transform.rotation.z,
           w = transform.rotation.w;
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 0.0) || !std::isfinite(length)) {
        x = y = z = 0.0;
        w = 1.0;
    } else {
        x /= length; y /= length; z /= length; w /= length;
    }
    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    const auto f = [](double v) { return static_cast<float>(v); };
    return {
        f(1.0 - 2.0 * (yy + zz)), f(2.0 * (xy + wz)), f(2.0 * (xz - wy)), 0.0F,
        f(2.0 * (xy - wz)), f(1.0 - 2.0 * (xx + zz)), f(2.0 * (yz + wx)), 0.0F,
        f(2.0 * (xz + wy)), f(2.0 * (yz - wx)), f(1.0 - 2.0 * (xx + yy)), 0.0F,
        transform.position.x, transform.position.y, transform.position.z, 1.0F};
}

[[nodiscard]] VoxelMaterialDefinition fallback_material(MaterialId id) {
    VoxelMaterialDefinition material;
    material.name = id == kAirMaterial ? "Air" : "Exported Material " + std::to_string(id);
    material.baseColor = id == kAirMaterial ? Float4{0, 0, 0, 0} : Float4{0.65F, 0.68F, 0.72F, 1.0F};
    material.densityKilogramsPerCubicMeter = id == kAirMaterial ? 0.0F : 1000.0F;
    material.structural = id != kAirMaterial;
    return material;
}

[[nodiscard]] std::string node_path(const EditorDocument& document, EditorObjectId id) {
    std::vector<std::string> names;
    std::set<EditorObjectId> visited;
    for (const EditorObject* object = document.find_object(id); object && visited.insert(object->id).second;
         object = object->parent ? document.find_object(*object->parent) : nullptr) {
        std::string name = object->name.empty() ? "object_" + std::to_string(object->id) : object->name;
        std::replace(name.begin(), name.end(), '/', '_');
        names.push_back(std::move(name));
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it) path += "/" + *it;
    return path;
}

[[nodiscard]] bool is_polygon_source(const std::filesystem::path& source) {
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".dmesh";
}

[[nodiscard]] std::filesystem::path default_project_root(const EditorDocument& document) {
    if (document.path().empty()) return std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::path folder = std::filesystem::absolute(document.path(), ec).parent_path();
    if (ec) folder = document.path().parent_path();
    for (std::filesystem::path cursor = folder; !cursor.empty(); cursor = cursor.parent_path()) {
        if (std::filesystem::is_regular_file(cursor / "project.dveproject", ec)) return cursor;
        if (cursor == cursor.parent_path()) break;
    }
    return folder;
}

[[nodiscard]] VoxelMaterialDefinition library_or_fallback(const EditorMaterialLibrary& materials, std::uint32_t id) {
    if (id <= std::numeric_limits<MaterialId>::max()) {
        if (const EditorMaterialEntry* entry = materials.find(static_cast<MaterialId>(id))) return entry->definition;
    }
    return fallback_material(1);
}

[[nodiscard]] Float4 clamp_color(Float4 c) noexcept {
    const auto clamp01 = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0F, 1.0F) : 0.0F; };
    return {clamp01(c.x), clamp01(c.y), clamp01(c.z), clamp01(c.w)};
}

struct VoxelRange {
    int minimum{};
    int maximum{}; // exclusive
};

// Voxels whose centre lies in [low, high]; at least one voxel (the one containing the midpoint).
[[nodiscard]] VoxelRange centre_range(float low, float high, float size) noexcept {
    VoxelRange range{static_cast<int>(std::ceil(low / size - 0.5F)), static_cast<int>(std::floor(high / size - 0.5F)) + 1};
    if (range.maximum <= range.minimum) {
        range.minimum = static_cast<int>(std::floor((low + high) * 0.5F / size));
        range.maximum = range.minimum + 1;
    }
    return range;
}

// Keeps the component list the runtime can take: GameWorld accepts any component that passes
// validate_components; known dve.* types must also match their schema; unknown dve.* types are
// engine-reserved names this runtime does not implement. dve.prefab_instance and
// dve.editor_scatter are editor-only.
std::vector<Component> exportable_components(const EditorObject& object, const ComponentTypeRegistry& registry,
                                             const std::function<void(std::string)>& warn,
                                             const std::string& label) {
    std::vector<Component> kept;
    bool hasMembership = false;
    for (const Component& component : object.components) {
        if (component.type == "dve.prefab_instance") continue;
        if (component.type == "dve.editor_scatter") continue;  // scatter settings: editor-only
        std::string reason;
        if (!validate_components(std::span<const Component>(&component, 1U), nullptr, &reason)) {
        } else if (component.type.starts_with("dve.")) {
            if (!registry.find(component.type)) reason = "unknown engine component type";
            else (void)registry.validate(component, &reason);
        }
        if (!reason.empty()) {
            warn(label + ": component " + std::to_string(component.id) + " '" + component.type +
                 "' dropped (" + reason + ")");
            continue;
        }
        if (dve::find_component(std::span<const Component>(kept), component.id)) {
            warn(label + ": component " + std::to_string(component.id) + " '" + component.type +
                 "' dropped (duplicate component id)");
            continue;
        }
        hasMembership = hasMembership || component.type == "dve.membership";
        kept.push_back(component);
    }
    if (!hasMembership && (!object.tags.empty() || !object.groups.empty() || object.layer != 0U)) {
        Component membership;
        membership.id = 1U;
        for (const Component& existing : kept) membership.id = std::max(membership.id, existing.id + 1U);
        membership.type = "dve.membership";
        membership.properties.emplace("tags", join_membership_values(object.tags));
        membership.properties.emplace("groups", join_membership_values(object.groups));
        membership.properties.emplace("layer", static_cast<std::int64_t>(object.layer));
        kept.push_back(std::move(membership));
    }
    return kept;
}

struct ExportItem {
    const EditorObject* object{};
    EditorSceneExportKind kind{EditorSceneExportKind::Voxel};
    std::optional<CookedVoxelAsset> baked;
    std::vector<char> polygonBytes;
    std::vector<Component> components;
    bool collision{true};
    bool structural{true};
    bool anchored{};
};

} // namespace

std::optional<CookedVoxelAsset> bake_text3d_voxels(
    const CookedText3DAsset& text, float voxelSizeMeters, const EditorMaterialLibrary& materials,
    std::uint64_t objectId, std::uint64_t maximumCells, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<CookedVoxelAsset> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (!(voxelSizeMeters > 0.0F) || !std::isfinite(voxelSizeMeters)) return fail("invalid voxel size");
    // The side mesh is four vertices per outline edge (a@z0, b@z0, b@z1, a@z1) in contour
    // order, so its a->b edges are exactly the flattened, directed glyph outlines.
    const auto& vertices = text.sideMesh.vertices;
    if (vertices.empty() || vertices.size() % 4U != 0U) return fail("the text has no glyph outlines to bake");
    struct Edge { Float2 a; Float2 b; };
    std::vector<Edge> edges;
    edges.reserve(vertices.size() / 4U);
    Float2 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Float2 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (std::size_t i = 0; i < vertices.size(); i += 4U) {
        const Edge edge{{vertices[i].position.x, vertices[i].position.y},
                        {vertices[i + 1U].position.x, vertices[i + 1U].position.y}};
        if (!std::isfinite(edge.a.x) || !std::isfinite(edge.a.y) || !std::isfinite(edge.b.x) || !std::isfinite(edge.b.y))
            return fail("the text outline has non-finite points");
        edges.push_back(edge);
        lo = {std::min({lo.x, edge.a.x, edge.b.x}), std::min({lo.y, edge.a.y, edge.b.y})};
        hi = {std::max({hi.x, edge.a.x, edge.b.x}), std::max({hi.y, edge.a.y, edge.b.y})};
    }
    const float halfDepth = std::max(0.0F, text.style.extrusionDepthMeters) * 0.5F;
    const VoxelRange rx = centre_range(lo.x, hi.x, voxelSizeMeters);
    const VoxelRange ry = centre_range(lo.y, hi.y, voxelSizeMeters);
    const VoxelRange rz = centre_range(-halfDepth, halfDepth, voxelSizeMeters);
    const std::uint64_t cells = static_cast<std::uint64_t>(rx.maximum - rx.minimum) *
                                static_cast<std::uint64_t>(ry.maximum - ry.minimum) *
                                static_cast<std::uint64_t>(rz.maximum - rz.minimum);
    if (cells > maximumCells) {
        return fail("the text needs " + std::to_string(cells) + " cells at this voxel size (limit " +
                    std::to_string(maximumCells) + ")");
    }
    const bool evenOdd = text.style.fillRule == Text3DFillRule::EvenOdd;
    CookedVoxelAsset asset(objectId);
    asset.voxelSizeMeters = voxelSizeMeters;
    VoxelMaterialDefinition face = library_or_fallback(materials, text.style.faceMaterialId);
    face.name = "Text Face";
    face.baseColor = clamp_color(text.style.faceColor);
    VoxelMaterialDefinition side = library_or_fallback(materials, text.style.sideMaterialId);
    side.name = "Text Side";
    side.baseColor = clamp_color(text.style.sideColor);
    asset.materials = {fallback_material(kAirMaterial), face, side};
    const int frontLayer = rz.maximum - 1;
    const int backLayer = rz.minimum;
    for (int j = ry.minimum; j < ry.maximum; ++j) {
        const float y = (static_cast<float>(j) + 0.5F) * voxelSizeMeters;
        for (int i = rx.minimum; i < rx.maximum; ++i) {
            const float x = (static_cast<float>(i) + 0.5F) * voxelSizeMeters;
            int winding = 0;
            int crossings = 0;
            for (const Edge& e : edges) {
                const bool upward = e.a.y <= y && e.b.y > y;
                const bool downward = e.b.y <= y && e.a.y > y;
                if (!upward && !downward) continue;
                const float left = (e.b.x - e.a.x) * (y - e.a.y) - (x - e.a.x) * (e.b.y - e.a.y);
                if (upward && left > 0.0F) { ++winding; ++crossings; }
                else if (downward && left < 0.0F) { --winding; ++crossings; }
            }
            const bool inside = evenOdd ? (crossings % 2) != 0 : winding != 0;
            if (!inside) continue;
            for (int k = rz.minimum; k < rz.maximum; ++k) {
                const MaterialId material = (k == frontLayer || k == backLayer) ? MaterialId{1} : MaterialId{2};
                (void)asset.object.set_voxel({i, j, k}, material);
            }
        }
    }
    if (asset.object.occupied_voxel_count() == 0U) {
        return fail("the text is thinner than one voxel at " + std::to_string(voxelSizeMeters) + " m");
    }
    return asset;
}

std::optional<CookedVoxelAsset> bake_gabor_volume_voxels(
    const GaborVolumeAsset& volume, float voxelSizeMeters, float opacityThreshold,
    std::uint64_t objectId, std::uint64_t maximumCells, std::uint64_t maximumEvaluations, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<CookedVoxelAsset> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (!(voxelSizeMeters > 0.0F) || !std::isfinite(voxelSizeMeters)) return fail("invalid voxel size");
    if (!(opacityThreshold > 0.0F && opacityThreshold < 1.0F)) return fail("opacity threshold must be in (0, 1)");
    std::string validation;
    if (!volume.validate(&validation)) return fail("invalid Gabor volume: " + validation);
    if (volume.primitives.empty()) return fail("the Gabor volume has no primitives");
    const VoxelRange rx = centre_range(volume.boundsMinimum.x, volume.boundsMaximum.x, voxelSizeMeters);
    const VoxelRange ry = centre_range(volume.boundsMinimum.y, volume.boundsMaximum.y, voxelSizeMeters);
    const VoxelRange rz = centre_range(volume.boundsMinimum.z, volume.boundsMaximum.z, voxelSizeMeters);
    const std::uint64_t cells = static_cast<std::uint64_t>(rx.maximum - rx.minimum) *
                                static_cast<std::uint64_t>(ry.maximum - ry.minimum) *
                                static_cast<std::uint64_t>(rz.maximum - rz.minimum);
    if (cells > maximumCells) {
        return fail("the volume needs " + std::to_string(cells) + " cells at this voxel size (limit " +
                    std::to_string(maximumCells) + ")");
    }
    if (cells > maximumEvaluations / std::max<std::uint64_t>(1U, volume.primitives.size())) {
        return fail("the volume needs more than " + std::to_string(maximumEvaluations) +
                    " density evaluations at this voxel size");
    }
    // Solid when a one-voxel slab would reach the opacity threshold: 1 - exp(-d * s) >= t.
    const float densityThreshold = -std::log(1.0F - opacityThreshold) / voxelSizeMeters;
    Float3 albedo{};
    for (const GaborVolumePrimitive& primitive : volume.primitives) albedo = add(albedo, primitive.albedo);
    albedo = multiply(albedo, 1.0F / static_cast<float>(volume.primitives.size()));
    const Float3 emission = multiply(volume.material.emissionColor, volume.material.emissionIntensity);
    VoxelMaterialDefinition material = fallback_material(1);
    material.name = "Gabor Volume";
    material.baseColor = clamp_color({volume.material.albedoTint.x * albedo.x + emission.x,
                                      volume.material.albedoTint.y * albedo.y + emission.y,
                                      volume.material.albedoTint.z * albedo.z + emission.z, 1.0F});
    material.emissive = emission;
    material.densityKilogramsPerCubicMeter = 100.0F;
    material.structural = false;
    CookedVoxelAsset asset(objectId);
    asset.voxelSizeMeters = voxelSizeMeters;
    asset.materials = {fallback_material(kAirMaterial), material};
    for (int k = rz.minimum; k < rz.maximum; ++k) {
        for (int j = ry.minimum; j < ry.maximum; ++j) {
            for (int i = rx.minimum; i < rx.maximum; ++i) {
                const Float3 centre{(static_cast<float>(i) + 0.5F) * voxelSizeMeters,
                                    (static_cast<float>(j) + 0.5F) * voxelSizeMeters,
                                    (static_cast<float>(k) + 0.5F) * voxelSizeMeters};
                if (evaluate_gabor_density(volume, centre) >= densityThreshold) (void)asset.object.set_voxel({i, j, k}, 1);
            }
        }
    }
    if (asset.object.occupied_voxel_count() == 0U) {
        return fail("no voxel reaches the opacity threshold " + std::to_string(opacityThreshold));
    }
    return asset;
}

EditorSceneExportResult export_editor_scene(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const std::filesystem::path& manifestPath,
    const EditorSceneExportOptions& options) {
    EditorSceneExportResult result;
    result.manifestPath = manifestPath;
    const auto fail = [&](std::string message) {
        result.success = false;
        result.error = std::move(message);
        return result;
    };
    const std::string manifestName = manifestPath.filename().string();
    if (!ends_with(manifestName, kManifestSuffix) || manifestName.size() == kManifestSuffix.size()) {
        return fail("output manifest must be named <name>.dvoxscene.json");
    }
    std::string validationError;
    if (!document.validate(&validationError)) return fail("editor scene is invalid: " + validationError);

    std::string objectDirectory = options.objectDirectory;
    if (objectDirectory.empty()) {
        objectDirectory = manifestName.substr(0, manifestName.size() - kManifestSuffix.size()) + ".objects";
    }
    const std::filesystem::path relativeObjectDirectory =
        std::filesystem::path(objectDirectory).lexically_normal();
    if (relativeObjectDirectory.is_absolute() || relativeObjectDirectory.empty() ||
        *relativeObjectDirectory.begin() == "..") {
        return fail("object directory must be a relative path inside the manifest folder");
    }
    const std::filesystem::path projectRoot =
        options.projectRoot.empty() ? default_project_root(document) : options.projectRoot;
    const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();

    const auto warn = [&](std::string message) { result.warnings.push_back(std::move(message)); };
    const auto note = [&](std::string message) { result.notes.push_back(std::move(message)); };
    const auto label = [](const EditorObject& object) {
        return "object " + std::to_string(object.id) + " '" + object.name + "'";
    };

    // Pass 1: decide which objects are exported (in id order, like EditorPlaySession), and
    // bake / read everything that is not a plain voxel object.
    std::vector<ExportItem> exported;
    std::map<EditorObjectId, std::size_t> indexOf;
    for (const auto& [id, object] : document.objects()) {
        ExportItem item;
        item.object = &object;
        item.collision = object.flags.collisionEnabled;
        item.structural = object.flags.structural;
        item.anchored = object.flags.anchored;
        std::string skip;
        if (!object.flags.visible) {
            skip = "is hidden (DVOXSCENE v1 has no disabled flag)";
        } else if (!(object.voxelSizeMeters > 0.0F) || !std::isfinite(object.voxelSizeMeters)) {
            return fail(label(object) + " has an invalid voxel size");
        } else if (object.is_text3d()) {
            item.kind = EditorSceneExportKind::BakedText3D;
            std::string bakeError;
            item.baked = bake_text3d_voxels(*object.text3d, object.voxelSizeMeters, materials, object.id,
                                            options.maximumBakeCells, &bakeError);
            if (!item.baked) skip = "is a 3D text object that could not be baked to voxels: " + bakeError;
            else note(label(object) + ": 3D text baked to " + std::to_string(item.baked->object.occupied_voxel_count()) +
                      " voxels at " + std::to_string(object.voxelSizeMeters) + " m");
        } else if (object.is_gabor_volume()) {
            item.kind = EditorSceneExportKind::BakedGaborVolume;
            std::string bakeError;
            item.baked = bake_gabor_volume_voxels(*object.gaborVolume, object.voxelSizeMeters,
                                                  options.gaborOpacityThreshold, object.id, options.maximumBakeCells,
                                                  options.maximumGaborEvaluations, &bakeError);
            if (!item.baked) {
                skip = "is a Gabor volume that could not be baked to voxels: " + bakeError;
            } else {
                // The runtime has no volume renderer or volume collision: the bake is an opaque,
                // visual-only stand-in.
                item.collision = false;
                item.structural = false;
                note(label(object) + ": Gabor volume baked to " +
                     std::to_string(item.baked->object.occupied_voxel_count()) + " visual-only voxels (opacity >= " +
                     std::to_string(options.gaborOpacityThreshold) + ")");
            }
        } else if (is_polygon_source(object.sourceAsset)) {
            item.kind = EditorSceneExportKind::Polygon;
            const std::filesystem::path source =
                object.sourceAsset.is_absolute() ? object.sourceAsset : projectRoot / object.sourceAsset;
            std::ifstream input(source, std::ios::binary);
            if (!input) {
                skip = "is a polygon object whose source " + source.generic_string() + " cannot be read";
            } else {
                item.polygonBytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                const auto* data = reinterpret_cast<const std::byte*>(item.polygonBytes.data());
                const PolygonAssetReadResult read = read_dmesh(std::span<const std::byte>(data, item.polygonBytes.size()));
                if (!read) skip = "is a polygon object whose source " + source.generic_string() + " is invalid: " + read.error;
            }
        } else if (!object.voxels || object.voxels->occupied_voxel_count() == 0U) {
            skip = "has no voxels";
        }
        if (!skip.empty()) {
            warn(label(object) + " skipped: it " + skip);
            ++result.skippedObjects;
            continue;
        }
        item.components = exportable_components(object, registry, warn, label(object));
        if (object.prefabLink) {
            warn(label(object) + ": prefab link flattened (the current voxels are exported)");
        }
        indexOf.emplace(id, exported.size());
        exported.push_back(std::move(item));
    }
    if (exported.empty()) return fail("the editor scene has no exportable objects");
    if (options.strict && !result.warnings.empty()) {
        return fail("strict export: " + result.warnings.front() +
                    (result.warnings.size() > 1U ? " (and " + std::to_string(result.warnings.size() - 1U) + " more)" : ""));
    }

    // Pass 2: write the object files.
    std::error_code ec;
    const std::filesystem::path manifestDirectory =
        manifestPath.parent_path().empty() ? std::filesystem::path(".") : manifestPath.parent_path();
    const std::filesystem::path objectFolder = manifestDirectory / relativeObjectDirectory;
    std::filesystem::create_directories(objectFolder, ec);
    if (ec) return fail("could not create " + objectFolder.string() + ": " + ec.message());

    std::set<std::string> written;
    for (std::size_t index = 0; index < exported.size(); ++index) {
        ExportItem& item = exported[index];
        const EditorObject& object = *item.object;
        EditorSceneExportObject summary;
        summary.id = object.id;
        summary.name = object.name;
        summary.index = index;
        summary.kind = item.kind;
        summary.anchored = item.anchored;
        summary.collision = item.collision;
        summary.componentCount = item.components.size();
        if (item.kind == EditorSceneExportKind::Polygon) {
            const std::string fileName = std::to_string(object.id) + ".dmesh";
            const std::filesystem::path target = objectFolder / fileName;
            const std::filesystem::path temporary = target.string() + ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output || !output.write(item.polygonBytes.data(), static_cast<std::streamsize>(item.polygonBytes.size())))
                    return fail("could not write " + temporary.string());
            }
            std::filesystem::rename(temporary, target, ec);
            if (ec) return fail("could not replace " + target.string() + ": " + ec.message());
            written.insert(fileName);
            summary.file = (relativeObjectDirectory / fileName).generic_string();
            result.objects.push_back(std::move(summary));
            continue;
        }
        CookedVoxelAsset asset(object.id);
        if (item.baked) {
            asset = std::move(*item.baked);
        } else {
            asset.voxelSizeMeters = object.voxelSizeMeters;
            MaterialId maximumMaterial = 0;
            std::set<MaterialId> used;
            for (const auto& entry : object.voxels->bricks()) {
                const Brick& brick = entry.second;
                brick.occupancy().for_each_set([&](std::uint16_t voxel) {
                    const MaterialId material = brick.material(voxel);
                    maximumMaterial = std::max(maximumMaterial, material);
                    used.insert(material);
                });
            }
            asset.materials.reserve(static_cast<std::size_t>(maximumMaterial) + 1U);
            std::vector<MaterialId> unknown;
            for (std::size_t id = 0; id <= maximumMaterial; ++id) {
                const auto materialId = static_cast<MaterialId>(id);
                if (const EditorMaterialEntry* entry = materials.find(materialId)) {
                    asset.materials.push_back(entry->definition);
                } else {
                    asset.materials.push_back(fallback_material(materialId));
                    if (materialId != kAirMaterial && used.contains(materialId)) unknown.push_back(materialId);
                }
            }
            if (!unknown.empty()) {
                std::string ids;
                for (const MaterialId id : unknown) ids += (ids.empty() ? "" : ",") + std::to_string(id);
                warn(label(object) + ": material id(s) " + ids +
                     " are not in the material library; a neutral material was written");
            }
            asset.object = std::move(*clone_voxel_object(*object.voxels));
        }
        const std::string fileName = std::to_string(object.id) + ".dvox";
        std::string writeError;
        if (!write_dvox(objectFolder / fileName, asset, {}, &writeError)) {
            return fail("could not write " + (objectFolder / fileName).string() + ": " + writeError);
        }
        written.insert(fileName);
        summary.file = (relativeObjectDirectory / fileName).generic_string();
        summary.voxelCount = asset.object.occupied_voxel_count();
        result.objects.push_back(std::move(summary));
    }
    if (options.strict && !result.warnings.empty()) {
        return fail("strict export: " + result.warnings.front());
    }
    // Remove stale object files from an earlier export of the same scene.
    for (std::filesystem::directory_iterator it(objectFolder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string extension = it->path().extension().string();
        if (it->is_regular_file() && (extension == ".dvox" || extension == ".dmesh") &&
            !written.contains(it->path().filename().string())) {
            std::error_code removeError;
            std::filesystem::remove(it->path(), removeError);
        }
    }

    // Pass 3: the manifest (written last, atomically replaced).
    std::ostringstream json;
    json << std::setprecision(std::numeric_limits<float>::max_digits10);
    const std::string sceneName = options.sceneName.empty() ? document.name() : options.sceneName;
    json << "{\n  \"format\": \"DVOXSCENE\",\n  \"version\": 1,\n  \"name\": \"" << json_escape(sceneName)
         << "\",\n  \"objects\": [\n";
    for (std::size_t index = 0; index < exported.size(); ++index) {
        const ExportItem& item = exported[index];
        const EditorObject& object = *item.object;
        EditorSceneExportObject& summary = result.objects[index];
        std::optional<std::size_t> parentIndex;
        std::optional<RuntimeSceneAttachment> attachment;
        if (object.parent) {
            const auto parent = indexOf.find(*object.parent);
            if (parent != indexOf.end()) {
                parentIndex = parent->second;
                summary.hasParent = true;
                if (object.attachment) {
                    attachment = RuntimeSceneAttachment{object.attachment->socket, object.attachment->inheritPosition,
                                                        object.attachment->inheritRotation};
                    summary.attached = true;
                }
            } else {
                warn(label(object) + ": parent " + std::to_string(*object.parent) +
                     " was not exported, so the object is written as a root");
            }
        }
        const RigidTransform transform = document.world_transform(object.id).value_or(object.transform);
        const std::array<float, 16> matrix = world_matrix(transform);
        json << "    {\"index\":" << index << ",\"id\":" << object.id
             << ",\"name\":\"" << json_escape(object.name) << "\""
             << ",\"nodePath\":\"" << json_escape(node_path(document, object.id)) << "\""
             << ",\"file\":\"" << json_escape(summary.file) << "\""
             << ",\"parent\":";
        if (parentIndex) json << *parentIndex; else json << "null";
        json << ",\"anchored\":" << (item.anchored ? "true" : "false")
             << ",\"structural\":" << (item.structural ? "true" : "false")
             << ",\"generateCollision\":" << (item.collision ? "true" : "false")
             << ",\"worldMatrix\":[";
        for (std::size_t element = 0; element < matrix.size(); ++element) {
            if (element != 0U) json << ',';
            json << (matrix[element] == 0.0F ? 0.0F : matrix[element]);
        }
        json << ']';
        const std::string extensions = dvoxscene_object_extensions_json(
            item.kind == EditorSceneExportKind::Polygon ? RuntimeSceneGeometry::Polygon : RuntimeSceneGeometry::Voxel,
            item.components, attachment);
        if (!extensions.empty()) json << ",\"extensions\":" << extensions;
        json << '}' << (index + 1U == exported.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    if (options.strict && !result.warnings.empty()) {
        return fail("strict export: " + result.warnings.back());
    }

    const std::filesystem::path temporary = manifestPath.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return fail("could not open " + temporary.string());
        output << json.str();
        if (!output) return fail("could not write " + temporary.string());
    }
    std::filesystem::rename(temporary, manifestPath, ec);
    if (ec) return fail("could not replace " + manifestPath.string() + ": " + ec.message());
    result.success = true;
    return result;
}

} // namespace dve::editor
