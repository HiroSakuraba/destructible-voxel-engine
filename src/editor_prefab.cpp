#include "dve/editor_prefab.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <unordered_set>

#include "dve/editor_command.hpp"

namespace dve::editor {
namespace {

std::uint64_t hash_bytes(std::uint64_t hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t hash_text(std::uint64_t hash, std::string_view text) noexcept {
    return hash_bytes(hash, text.data(), text.size());
}

template <class T>
std::uint64_t hash_value(std::uint64_t hash, const T& value) noexcept {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    return hash_bytes(hash, &value, sizeof(value));
}

std::uint64_t hash_float3(std::uint64_t hash, Float3 value) noexcept {
    hash = hash_value(hash, value.x);
    hash = hash_value(hash, value.y);
    return hash_value(hash, value.z);
}

std::uint64_t hash_quaternion(std::uint64_t hash, Quaternion value) noexcept {
    hash = hash_value(hash, value.x);
    hash = hash_value(hash, value.y);
    hash = hash_value(hash, value.z);
    return hash_value(hash, value.w);
}

std::uint64_t hash_component_value(std::uint64_t hash, const ComponentValue& value) noexcept {
    hash = hash_value(hash, static_cast<std::uint8_t>(value.index()));
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) hash = hash_text(hash, item);
        else if constexpr (std::is_same_v<T, Float3>) hash = hash_float3(hash, item);
        else if constexpr (std::is_same_v<T, Quaternion>) hash = hash_quaternion(hash, item);
        else hash = hash_value(hash, item);
    }, value);
    return hash;
}

std::uint64_t hash_voxels(std::uint64_t hash, const VoxelObject& voxels) noexcept {
    for (const auto& [key, brick] : voxels.bricks()) {
        hash = hash_value(hash, key.x);
        hash = hash_value(hash, key.y);
        hash = hash_value(hash, key.z);
        const Bitset512 occupancy = brick.occupancy();
        hash = hash_bytes(hash, occupancy.words.data(), occupancy.words.size() * sizeof(std::uint64_t));
        const auto materials = brick.materials();
        hash = hash_bytes(hash, materials.data(), materials.size() * sizeof(MaterialId));
    }
    return hash;
}

std::filesystem::path template_scene_path(const std::filesystem::path& prefabPath) {
    return prefabPath.parent_path() / (prefabPath.stem().string() + ".prefab_template.dvescene");
}

bool path_is_safe_relative(const std::filesystem::path& path) noexcept {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) if (part == "..") return false;
    return true;
}

RigidTransform inverse_rigid_transform_local(const RigidTransform& transform) noexcept {
    const Quaternion inverseRotation = conjugate(normalize(transform.rotation));
    return make_rigid_transform(
        rotate(inverseRotation, multiply(transform.position, -1.0F)), inverseRotation);
}

std::filesystem::path instance_prefab_reference(
    const EditorDocument& document, const std::filesystem::path& prefabPath) {
    if (!prefabPath.is_absolute()) return prefabPath.lexically_normal();
    if (!document.path().empty()) {
        std::error_code ec;
        const auto relative = std::filesystem::relative(prefabPath, document.path().parent_path(), ec);
        if (!ec && path_is_safe_relative(relative)) return relative.lexically_normal();
    }
    return prefabPath.filename();
}

void remap_embedded_object_id(EditorObject& object, EditorObjectId id) {
    if (!object.text3d) return;
    object.text3d->objectId = id;
    object.text3d->sideMesh.objectId = id;
    object.text3d->sideMesh.contentHash = polygon_asset_content_hash(object.text3d->sideMesh);
    object.text3d->contentHash = text3d_content_hash(*object.text3d);
}

std::optional<ComponentId> parse_component_id(std::string_view text) noexcept {
    ComponentId value{};
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0U) return std::nullopt;
    return value;
}

bool assign_bool(ComponentValue& value, bool* output) {
    if (const auto* typed = std::get_if<bool>(&value)) { *output = *typed; return true; }
    return false;
}

bool object_has_override(const EditorObject& object, std::string_view path) {
    if (!object.prefabLink) return false;
    return object.prefabLink->overrides.contains(std::string(path));
}

bool apply_template_property(EditorObject& object, std::string_view propertyPath,
                             const ComponentValue& value, std::string* error) {
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    if (!prefab_override_path_is_valid(propertyPath) || !component_value_is_finite(value))
        return fail("invalid prefab template override");
    if (propertyPath == "name") {
        if (const auto* typed = std::get_if<std::string>(&value)) { object.name = *typed; return true; }
    } else if (propertyPath == "transform.position") {
        if (const auto* typed = std::get_if<Float3>(&value)) { object.transform.position = *typed; return true; }
    } else if (propertyPath == "transform.rotation") {
        if (const auto* typed = std::get_if<Quaternion>(&value)) { object.transform.rotation = *typed; return true; }
    } else if (propertyPath.starts_with("flags.")) {
        ComponentValue mutableValue = value;
        bool typed{};
        if (!assign_bool(mutableValue, &typed)) return fail("flag override requires a boolean");
        const std::string_view field = propertyPath.substr(6U);
        if (field == "visible") object.flags.visible = typed;
        else if (field == "locked") object.flags.locked = typed;
        else if (field == "anchored") object.flags.anchored = typed;
        else if (field == "structural") object.flags.structural = typed;
        else if (field == "collision_enabled") object.flags.collisionEnabled = typed;
        else if (field == "decorative") object.flags.decorative = typed;
        else return fail("unknown prefab flag override");
        return true;
    } else if (propertyPath.starts_with("component:")) {
        const std::size_t dot = propertyPath.find('.', 10U);
        if (dot == std::string_view::npos) return fail("component override has no property name");
        const auto componentId = parse_component_id(propertyPath.substr(10U, dot - 10U));
        if (!componentId) return fail("component override has an invalid component id");
        Component* component = find_component(std::span<Component>(object.components), *componentId);
        if (!component) return fail("component override target does not exist");
        component->properties[std::string(propertyPath.substr(dot + 1U))] = value;
        return true;
    }
    return fail("prefab override value type does not match the target property");
}

bool write_prefab_asset(EditorPrefabAsset& prefab, const std::filesystem::path& prefabPath,
                        std::string* error) {
    try {
        prefab.assetPath = prefabPath;
        const std::filesystem::path scenePath = template_scene_path(prefabPath);
        prefab.contentHash = editor_prefab_content_hash(prefab);
        std::error_code ec;
        std::filesystem::create_directories(prefabPath.parent_path().empty() ? "." : prefabPath.parent_path(), ec);
        const EditorSceneSaveResult saved = prefab.templateDocument.save_transactional(scenePath);
        if (!saved.success) throw std::runtime_error("could not save prefab template scene: " + saved.error);
        std::filesystem::path parentReference;
        if (!prefab.parentPrefabAsset.empty()) {
            parentReference = std::filesystem::relative(prefab.parentPrefabAsset, prefabPath.parent_path(), ec);
            if (ec || !path_is_safe_relative(parentReference))
                throw std::runtime_error("prefab parent must resolve to a safe relative path");
        }
        const std::filesystem::path temporary = prefabPath.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not open temporary prefab manifest");
        output << "DVE_PREFAB 2\n";
        output << "name " << std::quoted(prefab.name) << "\n";
        output << "template_scene " << std::quoted(scenePath.filename().generic_string()) << "\n";
        output << "parent_prefab " << std::quoted(parentReference.generic_string()) << "\n";
        output << "inheritance_depth " << prefab.inheritanceDepth << "\n";
        output << "roots " << prefab.rootTemplateObjectIds.size();
        for (const EditorObjectId root : prefab.rootTemplateObjectIds) output << ' ' << root;
        output << "\ncontent_hash " << prefab.contentHash << "\n";
        output.flush();
        if (!output) throw std::runtime_error("failed to write prefab manifest");
        output.close();
        std::filesystem::rename(temporary, prefabPath, ec);
        if (ec) {
            std::filesystem::remove(prefabPath, ec);
            ec.clear();
            std::filesystem::rename(temporary, prefabPath, ec);
        }
        if (ec) throw std::runtime_error("could not commit prefab manifest: " + ec.message());
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

std::optional<EditorPrefabAsset> load_editor_prefab_impl(
    const std::filesystem::path& prefabPath, std::uint32_t recursionDepth,
    std::unordered_set<std::string>& visited, std::string* error) {
    try {
        if (recursionDepth > 8U) throw std::runtime_error("prefab inheritance depth exceeds 8");
        std::error_code ec;
        const std::string canonical = std::filesystem::weakly_canonical(prefabPath, ec).generic_string();
        const std::string keyPath = ec ? prefabPath.lexically_normal().generic_string() : canonical;
        if (!visited.insert(keyPath).second) throw std::runtime_error("prefab inheritance cycle detected");
        std::ifstream input(prefabPath, std::ios::binary);
        if (!input) throw std::runtime_error("could not open prefab manifest");
        std::string magic;
        std::uint32_t version{};
        input >> magic >> version;
        if (magic != "DVE_PREFAB" || (version != 1U && version != 2U))
            throw std::runtime_error("unsupported prefab format");
        std::string recordKey;
        std::string name;
        std::string templateScene;
        std::string parentPrefab;
        std::uint32_t inheritanceDepth{};
        std::size_t rootCount{};
        input >> recordKey >> std::quoted(name);
        if (!input || recordKey != "name") throw std::runtime_error("missing prefab name");
        input >> recordKey >> std::quoted(templateScene);
        if (!input || recordKey != "template_scene") throw std::runtime_error("missing prefab template scene");
        const std::filesystem::path relativeTemplate(templateScene);
        if (!path_is_safe_relative(relativeTemplate)) throw std::runtime_error("unsafe prefab template path");
        if (version >= 2U) {
            input >> recordKey >> std::quoted(parentPrefab);
            if (!input || recordKey != "parent_prefab") throw std::runtime_error("missing prefab parent");
            input >> recordKey >> inheritanceDepth;
            if (!input || recordKey != "inheritance_depth" || inheritanceDepth > 8U)
                throw std::runtime_error("invalid prefab inheritance depth");
        }
        input >> recordKey >> rootCount;
        if (!input || recordKey != "roots" || rootCount == 0U || rootCount > 4096U)
            throw std::runtime_error("invalid prefab root list");
        std::vector<EditorObjectId> roots(rootCount);
        for (EditorObjectId& root : roots) input >> root;
        std::uint64_t storedHash{};
        input >> recordKey >> storedHash;
        if (!input || recordKey != "content_hash") throw std::runtime_error("missing prefab content hash");
        std::filesystem::path parentPath;
        if (!parentPrefab.empty()) {
            const std::filesystem::path relativeParent(parentPrefab);
            if (!path_is_safe_relative(relativeParent)) throw std::runtime_error("unsafe prefab parent path");
            parentPath = (prefabPath.parent_path() / relativeParent).lexically_normal();
            std::string parentError;
            auto parent = load_editor_prefab_impl(parentPath, recursionDepth + 1U, visited, &parentError);
            if (!parent) throw std::runtime_error("could not load prefab parent: " + parentError);
            if (inheritanceDepth != parent->inheritanceDepth + 1U)
                throw std::runtime_error("prefab inheritance depth does not match parent");
        } else if (inheritanceDepth != 0U) {
            throw std::runtime_error("root prefab must have inheritance depth zero");
        }
        std::string sceneError;
        auto document = EditorDocument::load(prefabPath.parent_path() / relativeTemplate, &sceneError);
        if (!document) throw std::runtime_error("could not load prefab template scene: " + sceneError);
        for (const EditorObjectId root : roots) {
            const EditorObject* object = document->find_object(root);
            if (!object || object->parent) throw std::runtime_error("invalid prefab root object");
        }
        EditorPrefabAsset prefab(name);
        prefab.assetPath = prefabPath;
        prefab.templateDocument = std::move(*document);
        prefab.rootTemplateObjectIds = std::move(roots);
        prefab.parentPrefabAsset = std::move(parentPath);
        prefab.inheritanceDepth = inheritanceDepth;
        prefab.contentHash = editor_prefab_content_hash(prefab);
        if (prefab.contentHash != storedHash) throw std::runtime_error("prefab content hash mismatch");
        visited.erase(keyPath);
        return prefab;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

} // namespace

std::uint64_t editor_prefab_content_hash(const EditorPrefabAsset& prefab) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash = hash_text(hash, prefab.name);
    hash = hash_text(hash, prefab.parentPrefabAsset.filename().generic_string());
    hash = hash_value(hash, prefab.inheritanceDepth);
    for (const EditorObjectId root : prefab.rootTemplateObjectIds) hash = hash_value(hash, root);
    for (const auto& [id, object] : prefab.templateDocument.objects()) {
        hash = hash_value(hash, id);
        hash = hash_text(hash, object.name);
        hash = hash_value(hash, object.parent.value_or(0U));
        hash = hash_float3(hash, object.transform.position);
        hash = hash_quaternion(hash, object.transform.rotation);
        hash = hash_value(hash, object.flags.visible);
        hash = hash_value(hash, object.flags.locked);
        hash = hash_value(hash, object.flags.anchored);
        hash = hash_value(hash, object.flags.structural);
        hash = hash_value(hash, object.flags.collisionEnabled);
        hash = hash_value(hash, object.flags.decorative);
        hash = hash_value(hash, object.layer);
        for (const std::string& tag : object.tags) hash = hash_text(hash, tag);
        for (const std::string& group : object.groups) hash = hash_text(hash, group);
        hash = hash_text(hash, object.sourceAsset.generic_string());
        hash = hash_value(hash, object.voxelSizeMeters);
        if (object.attachment) {
            hash = hash_float3(hash, object.attachment->localTransform.position);
            hash = hash_quaternion(hash, object.attachment->localTransform.rotation);
            hash = hash_text(hash, object.attachment->socket);
            hash = hash_value(hash, object.attachment->inheritPosition);
            hash = hash_value(hash, object.attachment->inheritRotation);
        }
        for (const Component& component : object.components) {
            hash = hash_value(hash, component.id);
            hash = hash_text(hash, component.type);
            hash = hash_value(hash, component.enabled);
            for (const auto& [name, value] : component.properties) {
                hash = hash_text(hash, name);
                hash = hash_component_value(hash, value);
            }
        }
        if (object.voxels) hash = hash_voxels(hash, *object.voxels);
    }
    return hash;
}

EditorPrefabCaptureResult capture_editor_prefab(
    const EditorDocument& document,
    std::span<const EditorObjectId> roots,
    const std::filesystem::path& prefabPath,
    std::string prefabName) {
    EditorPrefabCaptureResult result;
    result.prefabPath = prefabPath;
    result.templateScenePath = template_scene_path(prefabPath);
    try {
        if (roots.empty()) throw std::runtime_error("prefab capture requires at least one root object");
        if (prefabPath.extension() != ".dveprefab") throw std::runtime_error("prefab path must use .dveprefab");
        const std::vector<EditorObjectId> closure = collect_editor_object_subtree_ids(document, roots);
        if (closure.empty()) throw std::runtime_error("prefab roots do not exist");
        for (const EditorObjectId root : roots) {
            if (!document.find_object(root)) throw std::runtime_error("prefab root does not exist");
        }
        const auto origin = document.world_transform(roots.front());
        if (!origin) throw std::runtime_error("prefab origin transform could not be resolved");
        if (prefabName.empty()) prefabName = prefabPath.stem().string();

        EditorPrefabAsset prefab(prefabName);
        prefab.assetPath = prefabPath;
        std::map<EditorObjectId, EditorObjectId> remap;
        EditorObjectId nextTemplateId = 1U;
        for (const EditorObjectId sourceId : closure) remap.emplace(sourceId, nextTemplateId++);
        std::set<EditorObjectId> rootSet(roots.begin(), roots.end());

        for (const EditorObjectId sourceId : closure) {
            const EditorObject* source = document.find_object(sourceId);
            if (!source) throw std::runtime_error("prefab source object disappeared during capture");
            const auto sourceWorld = document.world_transform(sourceId);
            if (!sourceWorld) throw std::runtime_error("prefab source transform could not be resolved");
            EditorObject object = clone_editor_object(*source);
            object.id = remap.at(sourceId);
            remap_embedded_object_id(object, object.id);
            object.voxels = clone_voxel_object(*source->voxels);
            object.prefabLink.reset(); // Captures are flattened but preserve authored components and geometry.
            if (source->parent && remap.contains(*source->parent)) {
                object.parent = remap.at(*source->parent);
                const auto parentWorld = document.world_transform(*source->parent);
                if (!parentWorld) throw std::runtime_error("prefab parent transform could not be resolved");
                EditorAttachment attachment = source->attachment.value_or(EditorAttachment{});
                attachment.localTransform = attachment_local_from_world(
                    *parentWorld, *sourceWorld, attachment.inheritPosition, attachment.inheritRotation);
                object.attachment = std::move(attachment);
                object.transform = relative_rigid_transform(*origin, *sourceWorld);
            } else {
                object.parent.reset();
                object.attachment.reset();
                object.transform = relative_rigid_transform(*origin, *sourceWorld);
                prefab.rootTemplateObjectIds.push_back(object.id);
            }
            prefab.templateDocument.add_object(std::move(object));
        }
        prefab.templateDocument.refresh_attachment_world_transforms();
        std::string writeError;
        if (!write_prefab_asset(prefab, prefabPath, &writeError)) throw std::runtime_error(writeError);
        result.templateScenePath = template_scene_path(prefabPath);
        result.success = true;
        result.objectCount = closure.size();
        result.contentHash = prefab.contentHash;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

std::optional<EditorPrefabAsset> load_editor_prefab(
    const std::filesystem::path& prefabPath,
    std::string* error) {
    std::unordered_set<std::string> visited;
    return load_editor_prefab_impl(prefabPath, 0U, visited, error);
}

EditorPrefabVariantResult create_editor_prefab_variant(
    const std::filesystem::path& parentPrefabPath,
    const std::filesystem::path& variantPrefabPath,
    std::string variantName,
    std::span<const EditorPrefabTemplateOverride> overrides) {
    EditorPrefabVariantResult result;
    result.prefabPath = variantPrefabPath;
    try {
        std::string loadError;
        auto parent = load_editor_prefab(parentPrefabPath, &loadError);
        if (!parent) throw std::runtime_error("could not load parent prefab: " + loadError);
        if (parent->inheritanceDepth >= 8U) throw std::runtime_error("prefab inheritance depth exceeds 8");
        EditorPrefabAsset variant(variantName.empty() ? variantPrefabPath.stem().string() : std::move(variantName));
        variant.assetPath = variantPrefabPath;
        variant.parentPrefabAsset = parentPrefabPath;
        variant.inheritanceDepth = parent->inheritanceDepth + 1U;
        variant.rootTemplateObjectIds = parent->rootTemplateObjectIds;
        variant.templateDocument = clone_editor_document(parent->templateDocument);
        for (const EditorPrefabTemplateOverride& item : overrides) {
            EditorObject* object = variant.templateDocument.find_object(item.templateObjectId);
            if (!object) throw std::runtime_error("variant override target does not exist");
            std::string overrideError;
            if (!apply_template_property(*object, item.propertyPath, item.value, &overrideError))
                throw std::runtime_error(overrideError);
        }
        std::string writeError;
        if (!write_prefab_asset(variant, variantPrefabPath, &writeError)) throw std::runtime_error(writeError);
        result.success = true;
        result.overrideCount = overrides.size();
        result.contentHash = variant.contentHash;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

EditorPrefabInstantiationResult instantiate_editor_prefab(
    EditorDocument& document,
    const EditorPrefabAsset& prefab,
    const RigidTransform& instanceTransform) {
    EditorPrefabInstantiationResult result;
    try {
        std::string validationError;
        if (!prefab.templateDocument.validate(&validationError))
            throw std::runtime_error("invalid prefab template: " + validationError);
        if (prefab.rootTemplateObjectIds.empty()) throw std::runtime_error("prefab has no root objects");
        const std::uint64_t instanceId = document.allocate_prefab_instance_id();
        std::map<EditorObjectId, EditorObjectId> remap;
        for (const auto& [templateId, object] : prefab.templateDocument.objects()) {
            (void)object;
            remap.emplace(templateId, document.allocate_object_id());
        }
        std::set<EditorObjectId> rootSet(prefab.rootTemplateObjectIds.begin(), prefab.rootTemplateObjectIds.end());
        const std::filesystem::path prefabReference = instance_prefab_reference(document, prefab.assetPath);
        for (const auto& [templateId, source] : prefab.templateDocument.objects()) {
            EditorObject object = clone_editor_object(source);
            object.id = remap.at(templateId);
            remap_embedded_object_id(object, object.id);
            object.voxels = clone_voxel_object(*source.voxels);
            object.prefabLink = EditorPrefabLink{
                prefabReference, instanceId, templateId, rootSet.contains(templateId), prefab.contentHash, {}};
            if (source.parent) object.parent = remap.at(*source.parent);
            if (rootSet.contains(templateId)) {
                object.parent.reset();
                object.attachment.reset();
                object.transform = compose_rigid_transforms(instanceTransform, source.transform);
            } else {
                object.transform = compose_rigid_transforms(instanceTransform, source.transform);
            }
            EditorObject& inserted = document.add_object(std::move(object));
            result.objectIds.push_back(inserted.id);
            if (rootSet.contains(templateId)) result.rootObjectIds.push_back(inserted.id);
        }
        document.refresh_attachment_world_transforms();
        for (const EditorObjectId rootId : result.rootObjectIds) {
            EditorObject* root = document.find_object(rootId);
            if (!root) continue;
            Component marker;
            marker.id = document.allocate_component_id(rootId);
            marker.type = "dve.prefab_instance";
            marker.properties.emplace("asset", prefabReference.generic_string());
            marker.properties.emplace("instance_id", static_cast<std::int64_t>(instanceId));
            std::string ignored;
            (void)document.add_component(rootId, std::move(marker), &ignored);
        }
        result.success = true;
        result.instanceId = instanceId;
    } catch (const std::exception& exception) {
        for (auto it = result.rootObjectIds.rbegin(); it != result.rootObjectIds.rend(); ++it)
            (void)document.remove_object(*it);
        result.objectIds.clear();
        result.rootObjectIds.clear();
        result.error = exception.what();
    }
    return result;
}


EditorPrefabInstanceUpdatePreview preview_editor_prefab_instance_update(
    const EditorDocument& document, std::uint64_t instanceId, const EditorPrefabAsset& prefab) {
    EditorPrefabInstanceUpdatePreview preview;
    preview.instanceId = instanceId;
    preview.currentSourceHash = prefab.contentHash;
    const auto ids = document.prefab_instance_objects(instanceId);
    if (ids.empty()) { preview.error = "prefab instance does not exist"; return preview; }
    std::map<EditorObjectId, const EditorObject*> byTemplate;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = document.find_object(id);
        if (!object || !object->prefabLink) continue;
        byTemplate.emplace(object->prefabLink->templateObjectId, object);
        if (preview.previousSourceHash == 0U) preview.previousSourceHash = object->prefabLink->sourceContentHash;
        else if (preview.previousSourceHash != object->prefabLink->sourceContentHash)
            preview.conflicts.push_back("instance objects disagree about their source content hash");
    }
    preview.stale = preview.previousSourceHash != prefab.contentHash;
    for (const auto& [templateId, source] : prefab.templateDocument.objects()) {
        (void)source;
        if (byTemplate.contains(templateId)) ++preview.updatedObjects;
        else ++preview.addedObjects;
    }
    for (const auto& [templateId, object] : byTemplate) {
        if (!prefab.templateDocument.find_object(templateId)) {
            if (object->prefabLink && !object->prefabLink->overrides.empty())
                preview.conflicts.push_back("removed template object " + std::to_string(templateId) + " has instance overrides");
            else ++preview.removedObjects;
        }
    }
    preview.success = preview.conflicts.empty();
    return preview;
}

static EditorPrefabInstanceUpdatePreview apply_editor_prefab_instance_update_in_place(
    EditorDocument& document, std::uint64_t instanceId, const EditorPrefabAsset& prefab,
    bool allowConflicts) {
    EditorPrefabInstanceUpdatePreview preview = preview_editor_prefab_instance_update(document, instanceId, prefab);
    if (!preview.error.empty()) return preview;
    if (!preview.conflicts.empty() && !allowConflicts) {
        preview.success = false;
        preview.error = "prefab source update has conflicts";
        return preview;
    }
    if (!preview.stale) { preview.success = true; return preview; }
    try {
        const auto instanceIds = document.prefab_instance_objects(instanceId);
        std::map<EditorObjectId, EditorObjectId> templateToInstance;
        std::filesystem::path prefabReference;
        RigidTransform instanceTransform{};
        bool haveTransform = false;
        for (const EditorObjectId id : instanceIds) {
            const EditorObject* object = document.find_object(id);
            if (!object || !object->prefabLink) continue;
            templateToInstance.emplace(object->prefabLink->templateObjectId, id);
            prefabReference = object->prefabLink->prefabAsset;
            if (object->prefabLink->instanceRoot && !haveTransform) {
                const EditorObject* source = prefab.templateDocument.find_object(object->prefabLink->templateObjectId);
                if (source) {
                    const auto world = document.world_transform(id);
                    if (world) { instanceTransform = compose_rigid_transforms(*world, inverse_rigid_transform_local(source->transform)); haveTransform = true; }
                }
            }
        }
        if (!haveTransform) instanceTransform = {};
        // Add missing template objects in dependency order. Template IDs are stable identifiers,
        // not an ordering guarantee, so a child may sort before its parent in the source map.
        std::set<EditorObjectId> pendingTemplateIds;
        for (const auto& [templateId, source] : prefab.templateDocument.objects()) {
            (void)source;
            if (!templateToInstance.contains(templateId)) pendingTemplateIds.insert(templateId);
        }
        while (!pendingTemplateIds.empty()) {
            bool madeProgress = false;
            for (auto iterator = pendingTemplateIds.begin(); iterator != pendingTemplateIds.end();) {
                const EditorObjectId templateId = *iterator;
                const EditorObject* source = prefab.templateDocument.find_object(templateId);
                if (!source) throw std::runtime_error("prefab update template object disappeared");
                if (source->parent && !templateToInstance.contains(*source->parent)) {
                    ++iterator;
                    continue;
                }
                EditorObject object = clone_editor_object(*source);
                object.id = document.allocate_object_id();
                remap_embedded_object_id(object, object.id);
                object.voxels = clone_voxel_object(*source->voxels);
                object.prefabLink = EditorPrefabLink{prefabReference, instanceId, templateId,
                    std::find(prefab.rootTemplateObjectIds.begin(), prefab.rootTemplateObjectIds.end(), templateId) != prefab.rootTemplateObjectIds.end(),
                    prefab.contentHash, {}};
                if (source->parent) object.parent = templateToInstance.at(*source->parent);
                else {
                    object.parent.reset();
                    object.attachment.reset();
                }
                object.transform = compose_rigid_transforms(instanceTransform, source->transform);
                const EditorObjectId newId = document.add_object(std::move(object)).id;
                templateToInstance.emplace(templateId, newId);
                iterator = pendingTemplateIds.erase(iterator);
                madeProgress = true;
            }
            if (!madeProgress)
                throw std::runtime_error("prefab update contains an unresolved parent dependency");
        }
        // Update existing data while preserving explicit instance overrides.
        for (const auto& [templateId, source] : prefab.templateDocument.objects()) {
            EditorObject* target = document.find_object(templateToInstance.at(templateId));
            if (!target || !target->prefabLink) continue;
            if (!object_has_override(*target, "name")) target->name = source.name;
            if (!object_has_override(*target, "flags.visible")) target->flags.visible = source.flags.visible;
            if (!object_has_override(*target, "flags.locked")) target->flags.locked = source.flags.locked;
            if (!object_has_override(*target, "flags.anchored")) target->flags.anchored = source.flags.anchored;
            if (!object_has_override(*target, "flags.structural")) target->flags.structural = source.flags.structural;
            if (!object_has_override(*target, "flags.collision_enabled")) target->flags.collisionEnabled = source.flags.collisionEnabled;
            if (!object_has_override(*target, "flags.decorative")) target->flags.decorative = source.flags.decorative;
            target->tags = source.tags;
            target->groups = source.groups;
            target->layer = source.layer;
            for (const Component& sourceComponent : source.components) {
                Component* targetComponent = find_component(std::span<Component>(target->components), sourceComponent.id);
                if (!targetComponent) target->components.push_back(sourceComponent);
                else {
                    targetComponent->type = sourceComponent.type;
                    targetComponent->enabled = sourceComponent.enabled;
                    for (const auto& [name, value] : sourceComponent.properties) {
                        const std::string overridePath = "component:" + std::to_string(sourceComponent.id) + "." + name;
                        if (!object_has_override(*target, overridePath)) targetComponent->properties[name] = value;
                    }
                }
            }
            if (source.parent) target->parent = templateToInstance.at(*source.parent);
            else target->parent.reset();
            target->attachment = source.attachment;
            if (target->prefabLink->instanceRoot) {
                if (!object_has_override(*target, "transform.position") && !object_has_override(*target, "transform.rotation"))
                    target->transform = compose_rigid_transforms(instanceTransform, source.transform);
            } else target->transform = compose_rigid_transforms(instanceTransform, source.transform);
            target->prefabLink->sourceContentHash = prefab.contentHash;
        }
        // Remove source-deleted objects only when they have no overrides, unless conflicts were accepted.
        for (const EditorObjectId id : instanceIds) {
            EditorObject* object = document.find_object(id);
            if (!object || !object->prefabLink || prefab.templateDocument.find_object(object->prefabLink->templateObjectId)) continue;
            if (!object->prefabLink->overrides.empty() && !allowConflicts) continue;
            (void)document.remove_object(id);
        }
        document.refresh_attachment_world_transforms();
        document.mark_dirty();
        preview.success = true;
        preview.error.clear();
    } catch (const std::exception& exception) {
        preview.success = false;
        preview.error = exception.what();
    }
    return preview;
}


EditorPrefabInstanceUpdatePreview apply_editor_prefab_instance_update(
    EditorDocument& document, std::uint64_t instanceId, const EditorPrefabAsset& prefab,
    bool allowConflicts) {
    EditorDocument staged = clone_editor_document(document);
    EditorPrefabInstanceUpdatePreview result = apply_editor_prefab_instance_update_in_place(
        staged, instanceId, prefab, allowConflicts);
    if (result.success) document = std::move(staged);
    return result;
}


InstantiateEditorPrefabCommand::InstantiateEditorPrefabCommand(
    EditorPrefabAsset prefab, RigidTransform instanceTransform, std::string label)
    : prefab_(std::move(prefab)), instanceTransform_(instanceTransform), label_(std::move(label)) {}

CommandResult InstantiateEditorPrefabCommand::execute(EditorDocument& document) {
    if (snapshots_.empty()) {
        result_ = instantiate_editor_prefab(document, prefab_, instanceTransform_);
        if (!result_.success)
            return CommandResult::fail(result_.error.empty() ? "failed to instantiate prefab" : result_.error);
        orderedIds_ = collect_editor_object_subtree_ids(document, result_.rootObjectIds);
        snapshots_.reserve(orderedIds_.size());
        for (const EditorObjectId id : orderedIds_) {
            const EditorObject* object = document.find_object(id);
            if (!object) return CommandResult::fail("instantiated prefab object disappeared");
            snapshots_.push_back(clone_editor_object(*object));
        }
        return CommandResult::ok();
    }
    for (const EditorObjectId id : orderedIds_)
        if (document.find_object(id)) return CommandResult::fail("prefab instance object id is already in use");
    try {
        for (const EditorObject& snapshot : snapshots_)
            document.add_object(clone_editor_object(snapshot));
        document.refresh_attachment_world_transforms();
        document.mark_dirty();
    } catch (const std::exception& exception) {
        for (auto it = result_.rootObjectIds.rbegin(); it != result_.rootObjectIds.rend(); ++it)
            if (document.find_object(*it)) (void)document.remove_object(*it);
        return CommandResult::fail(std::string("failed to restore prefab instance: ") + exception.what());
    }
    return CommandResult::ok();
}

CommandResult InstantiateEditorPrefabCommand::undo(EditorDocument& document) {
    if (result_.rootObjectIds.empty()) return CommandResult::fail("prefab instance has not been created");
    for (const EditorObjectId root : result_.rootObjectIds) {
        if (!document.find_object(root)) return CommandResult::fail("prefab instance root no longer exists");
    }
    for (const EditorObjectId root : result_.rootObjectIds) {
        if (!document.remove_object(root)) return CommandResult::fail("failed to remove prefab instance");
    }
    document.mark_dirty();
    return CommandResult::ok();
}

bool apply_editor_prefab_override(
    EditorDocument& document,
    EditorObjectId objectId,
    std::string propertyPath,
    ComponentValue value,
    std::string* error) {
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    EditorObject* object = document.find_object(objectId);
    if (!object || !object->prefabLink) return fail("object is not part of a prefab instance");
    if (!prefab_override_path_is_valid(propertyPath) || !component_value_is_finite(value))
        return fail("invalid prefab override");
    bool applied = false;
    if (propertyPath == "name") {
        if (const auto* typed = std::get_if<std::string>(&value)) { object->name = *typed; applied = true; }
    } else if (propertyPath == "transform.position") {
        if (const auto* typed = std::get_if<Float3>(&value)) {
            RigidTransform transform = object->transform;
            transform.position = *typed;
            applied = document.set_world_transform(objectId, transform);
        }
    } else if (propertyPath == "transform.rotation") {
        if (const auto* typed = std::get_if<Quaternion>(&value)) {
            RigidTransform transform = object->transform;
            transform.rotation = *typed;
            applied = document.set_world_transform(objectId, transform);
        }
    } else if (propertyPath.starts_with("flags.")) {
        bool typed{};
        if (assign_bool(value, &typed)) {
            const std::string_view field = std::string_view(propertyPath).substr(6U);
            if (field == "visible") object->flags.visible = typed;
            else if (field == "locked") object->flags.locked = typed;
            else if (field == "anchored") object->flags.anchored = typed;
            else if (field == "structural") object->flags.structural = typed;
            else if (field == "collision_enabled") object->flags.collisionEnabled = typed;
            else if (field == "decorative") object->flags.decorative = typed;
            else return fail("unknown prefab flag override");
            applied = true;
        }
    } else if (propertyPath.starts_with("component:")) {
        const std::size_t dot = propertyPath.find('.', 10U);
        if (dot == std::string::npos) return fail("component override has no property name");
        const auto componentId = parse_component_id(std::string_view(propertyPath).substr(10U, dot - 10U));
        if (!componentId) return fail("component override has an invalid component id");
        applied = document.set_component_property(
            objectId, *componentId, propertyPath.substr(dot + 1U), value, error);
    }
    if (!applied) return fail("prefab override value type does not match the target property");
    document.mark_dirty();
    return document.record_prefab_override(objectId, std::move(propertyPath), std::move(value), error);
}

} // namespace dve::editor
