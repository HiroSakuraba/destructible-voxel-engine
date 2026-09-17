#include "dve/editor_scene_composition.hpp"
#include "dve/editor_document.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <span>
#include <utility>

namespace dve::editor {
namespace {

void synchronize_editor_membership_from_component(EditorObject& object, const Component& component) {
    if (component.type != "dve.membership") return;
    if (const auto it = component.properties.find("tags"); it != component.properties.end())
        if (const auto* value = std::get_if<std::string>(&it->second)) object.tags = parse_membership_values(*value);
    if (const auto it = component.properties.find("groups"); it != component.properties.end())
        if (const auto* value = std::get_if<std::string>(&it->second)) object.groups = parse_membership_values(*value);
    if (const auto it = component.properties.find("layer"); it != component.properties.end())
        if (const auto* value = std::get_if<std::int64_t>(&it->second))
            object.layer = *value >= 0 && *value <= 63 ? static_cast<std::uint32_t>(*value) : 0U;
}

bool transform_is_finite(const RigidTransform& transform) noexcept {
    return std::isfinite(transform.position.x) && std::isfinite(transform.position.y) &&
           std::isfinite(transform.position.z) && std::isfinite(transform.rotation.x) &&
           std::isfinite(transform.rotation.y) && std::isfinite(transform.rotation.z) &&
           std::isfinite(transform.rotation.w) &&
           (transform.rotation.x * transform.rotation.x +
            transform.rotation.y * transform.rotation.y +
            transform.rotation.z * transform.rotation.z +
            transform.rotation.w * transform.rotation.w) > 0.0F;
}

} // namespace

bool EditorAttachment::validate(std::string* error) const {
    if (!transform_is_finite(localTransform)) {
        if (error) *error = "attachment local transform is not finite";
        return false;
    }
    if (socket.size() > 128U) {
        if (error) *error = "attachment socket name is too long";
        return false;
    }
    return true;
}

bool EditorPrefabLink::validate(std::string* error) const {
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    if (prefabAsset.empty()) return fail("prefab link has no asset path");
    if (prefabAsset.is_absolute()) return fail("prefab link path must be project-relative");
    if (instanceId == 0U || templateObjectId == 0U) return fail("prefab link identifiers must be non-zero");
    if (sourceContentHash == 0U) return fail("prefab link has no source content hash");
    for (const auto& [path, value] : overrides) {
        if (!prefab_override_path_is_valid(path)) return fail("invalid prefab override path");
        if (!component_value_is_finite(value)) return fail("prefab override value is not finite");
    }
    return true;
}

RigidTransform compose_attachment_transform(
    const RigidTransform& parentWorld,
    const EditorAttachment& attachment) noexcept {
    RigidTransform result = attachment.localTransform;
    if (attachment.inheritPosition) {
        result.position = add(
            parentWorld.position,
            attachment.inheritRotation
                ? rotate(parentWorld.rotation, attachment.localTransform.position)
                : attachment.localTransform.position);
    }
    if (attachment.inheritRotation) {
        result.rotation = normalize(multiply(parentWorld.rotation, attachment.localTransform.rotation));
    } else {
        result.rotation = normalize(attachment.localTransform.rotation);
    }
    return result;
}

RigidTransform attachment_local_from_world(
    const RigidTransform& parentWorld,
    const RigidTransform& childWorld,
    bool inheritPosition,
    bool inheritRotation) noexcept {
    RigidTransform local = childWorld;
    if (inheritPosition) {
        const Float3 delta = subtract(childWorld.position, parentWorld.position);
        local.position = inheritRotation
            ? rotate(conjugate(normalize(parentWorld.rotation)), delta)
            : delta;
    }
    if (inheritRotation) {
        local.rotation = normalize(multiply(
            conjugate(normalize(parentWorld.rotation)), normalize(childWorld.rotation)));
    } else {
        local.rotation = normalize(childWorld.rotation);
    }
    return local;
}

bool prefab_override_path_is_valid(std::string_view path) noexcept {
    if (path.empty() || path.size() > 256U || path.front() == '.' || path.back() == '.') return false;
    bool segmentHasCharacter = false;
    for (const char raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '.') {
            if (!segmentHasCharacter) return false;
            segmentHasCharacter = false;
            continue;
        }
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':';
        if (!valid) return false;
        segmentHasCharacter = true;
    }
    return segmentHasCharacter;
}


std::optional<RigidTransform> EditorDocument::world_transform(EditorObjectId id) const noexcept {
    std::set<EditorObjectId> visiting;
    const auto resolve = [&](const auto& self, EditorObjectId objectId) -> std::optional<RigidTransform> {
        const EditorObject* object = find_object(objectId);
        if (!object) return std::nullopt;
        if (!object->parent || !object->attachment) return object->transform;
        if (!visiting.insert(objectId).second) return std::nullopt;
        const auto parentWorld = self(self, *object->parent);
        visiting.erase(objectId);
        if (!parentWorld) return std::nullopt;
        return compose_attachment_transform(*parentWorld, *object->attachment);
    };
    return resolve(resolve, id);
}

bool EditorDocument::set_world_transform(EditorObjectId id, const RigidTransform& worldTransform) {
    EditorObject* object = find_object(id);
    if (!object || !component_value_is_finite(ComponentValue{worldTransform.position}) ||
        !component_value_is_finite(ComponentValue{worldTransform.rotation})) return false;
    object->transform = make_rigid_transform(worldTransform.position, worldTransform.rotation);
    if (object->parent && object->attachment) {
        const auto parentWorld = world_transform(*object->parent);
        if (!parentWorld) return false;
        object->attachment->localTransform = attachment_local_from_world(
            *parentWorld, object->transform,
            object->attachment->inheritPosition, object->attachment->inheritRotation);
    }
    dirty_ = true;
    refresh_attachment_world_transforms();
    return true;
}

bool EditorDocument::attach_object(
    EditorObjectId id, EditorObjectId parent, bool preserveWorldTransform,
    std::string socket, bool inheritPosition, bool inheritRotation, std::string* error) {
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    EditorObject* object = find_object(id);
    if (!object) return fail("attachment child does not exist");
    if (id == parent || !find_object(parent)) return fail("attachment parent does not exist");
    for (std::optional<EditorObjectId> cursor = parent; cursor;) {
        if (*cursor == id) return fail("attachment would create a hierarchy cycle");
        const EditorObject* ancestor = find_object(*cursor);
        cursor = ancestor ? ancestor->parent : std::nullopt;
    }
    const auto childWorld = world_transform(id);
    const auto parentWorld = world_transform(parent);
    if (!childWorld || !parentWorld) return fail("attachment transform could not be resolved");
    EditorAttachment attachment;
    attachment.socket = std::move(socket);
    attachment.inheritPosition = inheritPosition;
    attachment.inheritRotation = inheritRotation;
    attachment.localTransform = preserveWorldTransform
        ? attachment_local_from_world(*parentWorld, *childWorld, inheritPosition, inheritRotation)
        : object->transform;
    std::string attachmentError;
    if (!attachment.validate(&attachmentError)) return fail(std::move(attachmentError));
    object->parent = parent;
    object->attachment = std::move(attachment);
    dirty_ = true;
    refresh_attachment_world_transforms();
    return true;
}

bool EditorDocument::detach_object(EditorObjectId id, bool preserveWorldTransform, std::string* error) {
    EditorObject* object = find_object(id);
    if (!object) {
        if (error) *error = "attachment child does not exist";
        return false;
    }
    const auto world = world_transform(id);
    if (!world) {
        if (error) *error = "attachment transform could not be resolved";
        return false;
    }
    object->parent.reset();
    object->attachment.reset();
    if (preserveWorldTransform) object->transform = *world;
    dirty_ = true;
    refresh_attachment_world_transforms();
    return true;
}

void EditorDocument::refresh_attachment_world_transforms() noexcept {
    std::set<EditorObjectId> visiting;
    const auto resolve = [&](const auto& self, EditorObjectId id) -> std::optional<RigidTransform> {
        EditorObject* object = find_object(id);
        if (!object) return std::nullopt;
        if (!object->parent || !object->attachment) return object->transform;
        if (!visiting.insert(id).second) return std::nullopt;
        const auto parentWorld = self(self, *object->parent);
        visiting.erase(id);
        if (!parentWorld) return std::nullopt;
        object->transform = compose_attachment_transform(*parentWorld, *object->attachment);
        return object->transform;
    };
    for (const auto& [id, object] : objects_) {
        (void)object;
        (void)resolve(resolve, id);
    }
}

ComponentId EditorDocument::allocate_component_id(EditorObjectId objectId) const noexcept {
    const EditorObject* object = find_object(objectId);
    if (!object) return kInvalidComponentId;
    ComponentId candidate = 1U;
    for (const Component& component : object->components) candidate = std::max(candidate, component.id + 1U);
    while (candidate == kInvalidComponentId || dve::find_component(std::span<const Component>(object->components), candidate)) ++candidate;
    return candidate;
}

Component* EditorDocument::add_component(EditorObjectId objectId, Component component, std::string* error) {
    const auto fail = [&](std::string text) -> Component* {
        if (error) *error = std::move(text);
        return nullptr;
    };
    EditorObject* object = find_object(objectId);
    if (!object) return fail("component owner does not exist");
    if (component.id == kInvalidComponentId) component.id = allocate_component_id(objectId);
    if (dve::find_component(std::span<const Component>(object->components), component.id)) return fail("component id already exists on object");
    std::string validationError;
    if (!validate_components(std::span<const Component>(&component, 1U), nullptr, &validationError))
        return fail(std::move(validationError));
    object->components.push_back(std::move(component));
    synchronize_editor_membership_from_component(*object, object->components.back());
    dirty_ = true;
    return &object->components.back();
}

bool EditorDocument::remove_component(EditorObjectId objectId, ComponentId componentId) {
    EditorObject* object = find_object(objectId);
    if (!object) return false;
    const Component* target = dve::find_component(std::span<const Component>(object->components), componentId);
    if (target && target->type == "dve.membership") return false;
    const auto before = object->components.size();
    std::erase_if(object->components, [componentId](const Component& component) {
        return component.id == componentId;
    });
    if (object->components.size() == before) return false;
    dirty_ = true;
    return true;
}

Component* EditorDocument::find_component(EditorObjectId objectId, ComponentId componentId) noexcept {
    EditorObject* object = find_object(objectId);
    return object ? dve::find_component(std::span<Component>(object->components), componentId) : nullptr;
}

const Component* EditorDocument::find_component(EditorObjectId objectId, ComponentId componentId) const noexcept {
    const EditorObject* object = find_object(objectId);
    return object ? dve::find_component(std::span<const Component>(object->components), componentId) : nullptr;
}

bool EditorDocument::set_component_property(
    EditorObjectId objectId, ComponentId componentId, std::string property,
    ComponentValue value, std::string* error) {
    Component* component = find_component(objectId, componentId);
    if (!component) {
        if (error) *error = "component does not exist";
        return false;
    }
    if (!component_property_name_is_valid(property) || !component_value_is_finite(value)) {
        if (error) *error = "invalid component property";
        return false;
    }
    component->properties[std::move(property)] = std::move(value);
    if (EditorObject* object = find_object(objectId))
        synchronize_editor_membership_from_component(*object, *component);
    dirty_ = true;
    return true;
}

bool EditorDocument::set_component_enabled(EditorObjectId objectId, ComponentId componentId, bool enabled) {
    Component* component = find_component(objectId, componentId);
    if (!component) return false;
    if (component->enabled == enabled) return true;
    component->enabled = enabled;
    dirty_ = true;
    return true;
}

bool EditorDocument::reorder_component(EditorObjectId objectId, ComponentId componentId, std::size_t newIndex) {
    EditorObject* object = find_object(objectId);
    if (!object || newIndex >= object->components.size()) return false;
    const auto it = std::find_if(object->components.begin(), object->components.end(),
                                 [componentId](const Component& component) { return component.id == componentId; });
    if (it == object->components.end()) return false;
    const std::size_t oldIndex = static_cast<std::size_t>(std::distance(object->components.begin(), it));
    if (oldIndex == newIndex) return true;
    Component moved = std::move(*it);
    object->components.erase(it);
    object->components.insert(object->components.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(moved));
    dirty_ = true;
    return true;
}

bool EditorDocument::set_membership(
    EditorObjectId objectId, std::vector<std::string> tags, std::uint32_t layer,
    std::vector<std::string> groups, std::string* error) {
    EditorObject* object = find_object(objectId);
    if (!object) { if (error) *error = "object does not exist"; return false; }
    if (layer > 63U) { if (error) *error = "layer must be between 0 and 63"; return false; }
    object->tags = normalize_membership_values(tags);
    object->groups = normalize_membership_values(groups);
    object->layer = layer;
    Component* membership = dve::find_component_by_type(std::span<Component>(object->components), "dve.membership");
    if (!membership) {
        Component component;
        component.id = allocate_component_id(objectId);
        component.type = "dve.membership";
        component.properties.emplace("tags", join_membership_values(object->tags));
        component.properties.emplace("groups", join_membership_values(object->groups));
        component.properties.emplace("layer", static_cast<std::int64_t>(layer));
        object->components.push_back(std::move(component));
    } else {
        membership->properties["tags"] = join_membership_values(object->tags);
        membership->properties["groups"] = join_membership_values(object->groups);
        membership->properties["layer"] = static_cast<std::int64_t>(layer);
    }
    dirty_ = true;
    return true;
}

std::vector<EditorObjectId> EditorDocument::find_by_tag(std::string_view tag) const {
    std::vector<EditorObjectId> result;
    for (const auto& [id, object] : objects_)
        if (std::binary_search(object.tags.begin(), object.tags.end(), std::string(tag))) result.push_back(id);
    return result;
}

std::vector<EditorObjectId> EditorDocument::find_by_group(std::string_view group) const {
    std::vector<EditorObjectId> result;
    for (const auto& [id, object] : objects_)
        if (std::binary_search(object.groups.begin(), object.groups.end(), std::string(group))) result.push_back(id);
    return result;
}

std::vector<EditorObjectId> EditorDocument::find_by_layer(std::uint32_t layer) const {
    std::vector<EditorObjectId> result;
    for (const auto& [id, object] : objects_) if (object.layer == layer) result.push_back(id);
    return result;
}

std::uint64_t EditorDocument::allocate_prefab_instance_id() noexcept {
    while (nextPrefabInstanceId_ == 0U) ++nextPrefabInstanceId_;
    return nextPrefabInstanceId_++;
}

std::vector<EditorObjectId> EditorDocument::prefab_instance_objects(std::uint64_t instanceId) const {
    std::vector<EditorObjectId> result;
    for (const auto& [id, object] : objects_) {
        if (object.prefabLink && object.prefabLink->instanceId == instanceId) result.push_back(id);
    }
    return result;
}

bool EditorDocument::record_prefab_override(
    EditorObjectId objectId, std::string propertyPath, ComponentValue value, std::string* error) {
    EditorObject* object = find_object(objectId);
    if (!object || !object->prefabLink) {
        if (error) *error = "object is not part of a prefab instance";
        return false;
    }
    if (!prefab_override_path_is_valid(propertyPath) || !component_value_is_finite(value)) {
        if (error) *error = "invalid prefab override";
        return false;
    }
    object->prefabLink->overrides[std::move(propertyPath)] = std::move(value);
    dirty_ = true;
    return true;
}

bool EditorDocument::clear_prefab_override(EditorObjectId objectId, std::string_view propertyPath) {
    EditorObject* object = find_object(objectId);
    if (!object || !object->prefabLink) return false;
    if (object->prefabLink->overrides.erase(std::string(propertyPath)) == 0U) return false;
    dirty_ = true;
    return true;
}

} // namespace dve::editor
