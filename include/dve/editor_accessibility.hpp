#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace dve::editor {

class NativeEditorController;

enum class AccessibilityRole {
    Application,
    MenuBar,
    Menu,
    MenuItem,
    ToolBar,
    Button,
    Tree,
    TreeItem,
    Viewport,
    Inspector,
    Panel,
    StatusText,
};

struct AccessibilityNode {
    std::string id;
    AccessibilityRole role{AccessibilityRole::Panel};
    std::string label;
    std::string description;
    std::string shortcut;
    bool enabled{true};
    bool focused{};
    bool selected{};
    std::vector<AccessibilityNode> children;
};

[[nodiscard]] AccessibilityNode build_editor_accessibility_tree(
    const NativeEditorController& controller);
[[nodiscard]] std::string serialize_accessibility_tree_json(
    const AccessibilityNode& root);
[[nodiscard]] bool save_accessibility_tree_json(
    const std::filesystem::path& path,
    const AccessibilityNode& root,
    std::string* error = nullptr);

} // namespace dve::editor
