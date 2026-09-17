#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/editor_command.hpp"

namespace dve::editor {

struct EditorPrefabAsset {
    std::filesystem::path assetPath;
    std::string name;
    EditorDocument templateDocument;
    std::vector<EditorObjectId> rootTemplateObjectIds;
    std::filesystem::path parentPrefabAsset;
    std::uint32_t inheritanceDepth{};
    std::uint64_t contentHash{};

    explicit EditorPrefabAsset(std::string prefabName = "Prefab")
        : name(std::move(prefabName)), templateDocument(name) {}
};

struct EditorPrefabCaptureResult {
    bool success{};
    std::filesystem::path prefabPath;
    std::filesystem::path templateScenePath;
    std::size_t objectCount{};
    std::uint64_t contentHash{};
    std::string error;
};


struct EditorPrefabTemplateOverride {
    EditorObjectId templateObjectId{};
    std::string propertyPath;
    ComponentValue value;
};

struct EditorPrefabVariantResult {
    bool success{};
    std::filesystem::path prefabPath;
    std::size_t overrideCount{};
    std::uint64_t contentHash{};
    std::string error;
};

struct EditorPrefabInstanceUpdatePreview {
    bool success{};
    bool stale{};
    std::uint64_t instanceId{};
    std::uint64_t previousSourceHash{};
    std::uint64_t currentSourceHash{};
    std::size_t updatedObjects{};
    std::size_t addedObjects{};
    std::size_t removedObjects{};
    std::vector<std::string> conflicts;
    std::string error;
};

struct EditorPrefabInstantiationResult {
    bool success{};
    std::uint64_t instanceId{};
    std::vector<EditorObjectId> rootObjectIds;
    std::vector<EditorObjectId> objectIds;
    std::string error;
};

[[nodiscard]] EditorPrefabCaptureResult capture_editor_prefab(
    const EditorDocument& document,
    std::span<const EditorObjectId> roots,
    const std::filesystem::path& prefabPath,
    std::string prefabName = {});

[[nodiscard]] std::optional<EditorPrefabAsset> load_editor_prefab(
    const std::filesystem::path& prefabPath,
    std::string* error = nullptr);

[[nodiscard]] EditorPrefabVariantResult create_editor_prefab_variant(
    const std::filesystem::path& parentPrefabPath,
    const std::filesystem::path& variantPrefabPath,
    std::string variantName,
    std::span<const EditorPrefabTemplateOverride> overrides = {});

[[nodiscard]] EditorPrefabInstanceUpdatePreview preview_editor_prefab_instance_update(
    const EditorDocument& document,
    std::uint64_t instanceId,
    const EditorPrefabAsset& prefab);

[[nodiscard]] EditorPrefabInstanceUpdatePreview apply_editor_prefab_instance_update(
    EditorDocument& document,
    std::uint64_t instanceId,
    const EditorPrefabAsset& prefab,
    bool allowConflicts = false);

[[nodiscard]] EditorPrefabInstantiationResult instantiate_editor_prefab(
    EditorDocument& document,
    const EditorPrefabAsset& prefab,
    const RigidTransform& instanceTransform = {});


class InstantiateEditorPrefabCommand final : public IEditorCommand {
public:
    InstantiateEditorPrefabCommand(EditorPrefabAsset prefab,
                                   RigidTransform instanceTransform = {},
                                   std::string label = "Instantiate prefab");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
    [[nodiscard]] const EditorPrefabInstantiationResult& result() const noexcept { return result_; }
private:
    EditorPrefabAsset prefab_;
    RigidTransform instanceTransform_{};
    EditorPrefabInstantiationResult result_;
    std::vector<EditorObject> snapshots_;
    std::vector<EditorObjectId> orderedIds_;
    std::string label_;
};

[[nodiscard]] bool apply_editor_prefab_override(
    EditorDocument& document,
    EditorObjectId objectId,
    std::string propertyPath,
    ComponentValue value,
    std::string* error = nullptr);

[[nodiscard]] std::uint64_t editor_prefab_content_hash(const EditorPrefabAsset& prefab) noexcept;

} // namespace dve::editor
