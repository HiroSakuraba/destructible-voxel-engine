#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "dve/component.hpp"
#include "dve/transform.hpp"

namespace dve::editor {

struct EditorAttachment {
    RigidTransform localTransform{};
    std::string socket;
    bool inheritPosition{true};
    bool inheritRotation{true};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct EditorPrefabOverride {
    std::string propertyPath;
    ComponentValue value{};
};

struct EditorPrefabLink {
    std::filesystem::path prefabAsset;
    std::uint64_t instanceId{};
    std::uint64_t templateObjectId{};
    bool instanceRoot{};
    std::uint64_t sourceContentHash{};
    std::map<std::string, ComponentValue, std::less<>> overrides;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] RigidTransform compose_attachment_transform(
    const RigidTransform& parentWorld,
    const EditorAttachment& attachment) noexcept;
[[nodiscard]] RigidTransform attachment_local_from_world(
    const RigidTransform& parentWorld,
    const RigidTransform& childWorld,
    bool inheritPosition = true,
    bool inheritRotation = true) noexcept;

[[nodiscard]] bool prefab_override_path_is_valid(std::string_view path) noexcept;

} // namespace dve::editor
