#include "dve/editor_scene_export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

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

} // namespace

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

    const auto warn = [&](std::string message) { result.warnings.push_back(std::move(message)); };
    const auto label = [](const EditorObject& object) {
        return "object " + std::to_string(object.id) + " '" + object.name + "'";
    };

    // Pass 1: decide which objects are exported (in id order, like EditorPlaySession).
    std::vector<const EditorObject*> exported;
    std::map<EditorObjectId, std::size_t> indexOf;
    for (const auto& [id, object] : document.objects()) {
        std::string skip;
        if (!object.flags.visible) {
            skip = "is hidden (DVOXSCENE v1 has no disabled flag)";
        } else if (object.is_text3d()) {
            skip = "is a 3D text object (not supported by DVOXSCENE v1)";
        } else if (object.is_gabor_volume()) {
            skip = "is a Gabor volume (not supported by DVOXSCENE v1)";
        } else if (is_polygon_source(object.sourceAsset)) {
            skip = "is a polygon (.dmesh) object (not supported by DVOXSCENE v1)";
        } else if (!object.voxels || object.voxels->occupied_voxel_count() == 0U) {
            skip = "has no voxels";
        }
        if (!skip.empty()) {
            warn(label(object) + " skipped: it " + skip);
            ++result.skippedObjects;
            continue;
        }
        if (!object.components.empty()) {
            warn(label(object) + ": " + std::to_string(object.components.size()) +
                 " component(s) dropped (DVOXSCENE v1 has no components)");
        }
        if (!object.tags.empty() || !object.groups.empty() || object.layer != 0U) {
            warn(label(object) + ": tags/groups/layer dropped (DVOXSCENE v1 has no membership)");
        }
        if (object.prefabLink) {
            warn(label(object) + ": prefab link flattened (the current voxels are exported)");
        }
        if (!(object.voxelSizeMeters > 0.0F) || !std::isfinite(object.voxelSizeMeters)) {
            return fail(label(object) + " has an invalid voxel size");
        }
        indexOf.emplace(id, exported.size());
        exported.push_back(&object);
    }
    if (exported.empty()) return fail("the editor scene has no exportable voxel objects");
    if (options.strict && !result.warnings.empty()) {
        return fail("strict export: " + result.warnings.front() +
                    (result.warnings.size() > 1U ? " (and " + std::to_string(result.warnings.size() - 1U) + " more)" : ""));
    }

    // Pass 2: write the .dvox files.
    std::error_code ec;
    const std::filesystem::path manifestDirectory =
        manifestPath.parent_path().empty() ? std::filesystem::path(".") : manifestPath.parent_path();
    const std::filesystem::path objectFolder = manifestDirectory / relativeObjectDirectory;
    std::filesystem::create_directories(objectFolder, ec);
    if (ec) return fail("could not create " + objectFolder.string() + ": " + ec.message());

    std::set<std::string> written;
    for (std::size_t index = 0; index < exported.size(); ++index) {
        const EditorObject& object = *exported[index];
        CookedVoxelAsset asset(object.id);
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
        const std::string fileName = std::to_string(object.id) + ".dvox";
        std::string writeError;
        if (!write_dvox(objectFolder / fileName, asset, {}, &writeError)) {
            return fail("could not write " + (objectFolder / fileName).string() + ": " + writeError);
        }
        written.insert(fileName);

        EditorSceneExportObject summary;
        summary.id = object.id;
        summary.name = object.name;
        summary.file = (relativeObjectDirectory / fileName).generic_string();
        summary.index = index;
        summary.voxelCount = object.voxels->occupied_voxel_count();
        summary.anchored = object.flags.anchored;
        summary.collision = object.flags.collisionEnabled;
        result.objects.push_back(std::move(summary));
    }
    if (options.strict && !result.warnings.empty()) {
        return fail("strict export: " + result.warnings.front());
    }
    // Remove stale .dvox files from an earlier export of the same scene.
    for (std::filesystem::directory_iterator it(objectFolder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".dvox" &&
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
        const EditorObject& object = *exported[index];
        EditorSceneExportObject& summary = result.objects[index];
        std::optional<std::size_t> parentIndex;
        if (object.parent) {
            const auto parent = indexOf.find(*object.parent);
            if (parent != indexOf.end()) {
                parentIndex = parent->second;
                summary.hasParent = true;
            } else {
                warn(label(object) + ": parent " + std::to_string(*object.parent) +
                     " was not exported, so the object is written as a root");
            }
            if (object.attachment) {
                warn(label(object) + ": attachment exported as hierarchy metadata only "
                     "(the player does not attach bodies yet)");
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
        json << ",\"anchored\":" << (object.flags.anchored ? "true" : "false")
             << ",\"structural\":" << (object.flags.structural ? "true" : "false")
             << ",\"generateCollision\":" << (object.flags.collisionEnabled ? "true" : "false")
             << ",\"worldMatrix\":[";
        for (std::size_t element = 0; element < matrix.size(); ++element) {
            if (element != 0U) json << ',';
            json << (matrix[element] == 0.0F ? 0.0F : matrix[element]);
        }
        json << "]}" << (index + 1U == exported.size() ? "\n" : ",\n");
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
