#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/component.hpp"
#include "dve/editor_scene_composition.hpp"
#include "dve/gabor_volume.hpp"
#include "dve/transform.hpp"
#include "dve/text3d.hpp"
#include "dve/voxel_object.hpp"

namespace dve::editor {

using EditorObjectId = std::uint64_t;
class EditorDocument;

struct EditorObjectFlags {
    bool visible{true};
    bool locked{};
    bool anchored{};
    bool structural{true};
    bool collisionEnabled{true};
    bool decorative{};
    auto operator<=>(const EditorObjectFlags&) const = default;
};

struct EditorObject {
    EditorObjectId id{};
    std::string name;
    std::optional<EditorObjectId> parent;
    // World-space cache. Attached objects derive this from attachment.localTransform and the
    // parent world transform; unattached objects author it directly.
    RigidTransform transform{};
    std::optional<EditorAttachment> attachment;
    EditorObjectFlags flags{};
    std::filesystem::path sourceAsset;
    std::filesystem::path importRecipe;
    // Original project-owned font used to recook an embedded .dtext object. The cooked asset
    // stores only the used glyph subset and never embeds the font itself.
    std::filesystem::path textFontAsset;
    float voxelSizeMeters{0.10F};
    std::unique_ptr<VoxelObject> voxels;
    std::optional<CookedText3DAsset> text3d;
    std::optional<GaborVolumeAsset> gaborVolume;
    std::set<Int3> anchors;
    std::vector<Component> components;
    std::vector<std::string> tags;
    std::vector<std::string> groups;
    std::uint32_t layer{};
    std::optional<EditorPrefabLink> prefabLink;

    [[nodiscard]] bool is_text3d() const noexcept { return text3d.has_value(); }
    [[nodiscard]] bool is_gabor_volume() const noexcept { return gaborVolume.has_value(); }

    EditorObject();
    explicit EditorObject(EditorObjectId objectId, std::string objectName = {});
    EditorObject(EditorObject&&) noexcept = default;
    EditorObject& operator=(EditorObject&&) noexcept = default;
    EditorObject(const EditorObject&) = delete;
    EditorObject& operator=(const EditorObject&) = delete;
};

[[nodiscard]] std::unique_ptr<VoxelObject> clone_voxel_object(const VoxelObject& source);
[[nodiscard]] EditorObject clone_editor_object(const EditorObject& source);
[[nodiscard]] EditorDocument clone_editor_document(const EditorDocument& source);

struct EditorSceneSaveResult {
    bool success{};
    std::filesystem::path manifestPath;
    std::filesystem::path revisionDirectory;
    std::string error;
};

class EditorDocument {
public:
    explicit EditorDocument(std::string name = "Untitled");

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] EditorObjectId allocate_object_id() noexcept;
    EditorObject& add_object(EditorObject object);
    [[nodiscard]] bool remove_object(EditorObjectId id);
    [[nodiscard]] bool reparent(EditorObjectId id, std::optional<EditorObjectId> parent);
    [[nodiscard]] std::optional<RigidTransform> world_transform(EditorObjectId id) const noexcept;
    [[nodiscard]] bool set_world_transform(EditorObjectId id, const RigidTransform& worldTransform);
    [[nodiscard]] bool attach_object(
        EditorObjectId id, EditorObjectId parent, bool preserveWorldTransform = true,
        std::string socket = {}, bool inheritPosition = true, bool inheritRotation = true,
        std::string* error = nullptr);
    [[nodiscard]] bool detach_object(EditorObjectId id, bool preserveWorldTransform = true,
                                     std::string* error = nullptr);
    void refresh_attachment_world_transforms() noexcept;

    [[nodiscard]] ComponentId allocate_component_id(EditorObjectId objectId) const noexcept;
    [[nodiscard]] Component* add_component(EditorObjectId objectId, Component component,
                                           std::string* error = nullptr);
    [[nodiscard]] bool remove_component(EditorObjectId objectId, ComponentId componentId);
    [[nodiscard]] Component* find_component(EditorObjectId objectId, ComponentId componentId) noexcept;
    [[nodiscard]] const Component* find_component(EditorObjectId objectId, ComponentId componentId) const noexcept;
    [[nodiscard]] bool set_component_property(EditorObjectId objectId, ComponentId componentId,
                                              std::string property, ComponentValue value,
                                              std::string* error = nullptr);
    [[nodiscard]] bool set_component_enabled(EditorObjectId objectId, ComponentId componentId, bool enabled);
    [[nodiscard]] bool reorder_component(EditorObjectId objectId, ComponentId componentId, std::size_t newIndex);
    [[nodiscard]] bool set_membership(EditorObjectId objectId, std::vector<std::string> tags,
                                      std::uint32_t layer, std::vector<std::string> groups,
                                      std::string* error = nullptr);
    [[nodiscard]] std::vector<EditorObjectId> find_by_tag(std::string_view tag) const;
    [[nodiscard]] std::vector<EditorObjectId> find_by_group(std::string_view group) const;
    [[nodiscard]] std::vector<EditorObjectId> find_by_layer(std::uint32_t layer) const;

    [[nodiscard]] std::uint64_t allocate_prefab_instance_id() noexcept;
    [[nodiscard]] std::vector<EditorObjectId> prefab_instance_objects(std::uint64_t instanceId) const;
    [[nodiscard]] bool record_prefab_override(EditorObjectId objectId, std::string propertyPath,
                                              ComponentValue value, std::string* error = nullptr);
    [[nodiscard]] bool clear_prefab_override(EditorObjectId objectId, std::string_view propertyPath);
    [[nodiscard]] EditorObject* find_object(EditorObjectId id) noexcept;
    [[nodiscard]] const EditorObject* find_object(EditorObjectId id) const noexcept;
    [[nodiscard]] std::vector<EditorObjectId> root_objects() const;
    [[nodiscard]] std::vector<EditorObjectId> children_of(EditorObjectId parent) const;
    [[nodiscard]] const std::map<EditorObjectId, EditorObject>& objects() const noexcept { return objects_; }

    void mark_dirty() noexcept { dirty_ = true; }
    void mark_clean() noexcept { dirty_ = false; }
    [[nodiscard]] bool validate(std::string* error = nullptr) const;

    EditorSceneSaveResult save_transactional(const std::filesystem::path& manifestPath);
    [[nodiscard]] static std::optional<EditorDocument> load(
        const std::filesystem::path& manifestPath,
        std::string* error = nullptr);

private:
    friend EditorDocument clone_editor_document(const EditorDocument& source);
    std::string name_;
    std::filesystem::path path_;
    std::map<EditorObjectId, EditorObject> objects_;
    EditorObjectId nextObjectId_{1};
    std::uint64_t revision_{};
    std::uint64_t nextPrefabInstanceId_{1U};
    bool dirty_{};
};

} // namespace dve::editor
