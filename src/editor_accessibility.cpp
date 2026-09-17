#include "dve/editor_accessibility.hpp"

#include <fstream>
#include <sstream>

#include "dve/editor_native.hpp"

namespace dve::editor {
namespace {

const char* role_name(AccessibilityRole role) noexcept {
    switch (role) {
        case AccessibilityRole::Application: return "application";
        case AccessibilityRole::MenuBar: return "menuBar";
        case AccessibilityRole::Menu: return "menu";
        case AccessibilityRole::MenuItem: return "menuItem";
        case AccessibilityRole::ToolBar: return "toolBar";
        case AccessibilityRole::Button: return "button";
        case AccessibilityRole::Tree: return "tree";
        case AccessibilityRole::TreeItem: return "treeItem";
        case AccessibilityRole::Viewport: return "viewport";
        case AccessibilityRole::Inspector: return "inspector";
        case AccessibilityRole::Panel: return "panel";
        case AccessibilityRole::StatusText: return "status";
    }
    return "panel";
}

std::string escape_json(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(hex[(c >> 4U) & 0xFU]);
                    result.push_back(hex[c & 0xFU]);
                } else result.push_back(static_cast<char>(c));
        }
    }
    return result;
}

void write_node(std::ostringstream& out, const AccessibilityNode& node, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    const std::string childPad(static_cast<std::size_t>(indent + 2), ' ');
    out << pad << "{\n"
        << childPad << "\"id\": \"" << escape_json(node.id) << "\",\n"
        << childPad << "\"role\": \"" << role_name(node.role) << "\",\n"
        << childPad << "\"label\": \"" << escape_json(node.label) << "\",\n"
        << childPad << "\"description\": \"" << escape_json(node.description) << "\",\n"
        << childPad << "\"shortcut\": \"" << escape_json(node.shortcut) << "\",\n"
        << childPad << "\"enabled\": " << (node.enabled ? "true" : "false") << ",\n"
        << childPad << "\"focused\": " << (node.focused ? "true" : "false") << ",\n"
        << childPad << "\"selected\": " << (node.selected ? "true" : "false") << ",\n"
        << childPad << "\"children\": [";
    if (!node.children.empty()) out << '\n';
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        write_node(out, node.children[index], indent + 4);
        if (index + 1 < node.children.size()) out << ',';
        out << '\n';
    }
    if (!node.children.empty()) out << childPad;
    out << "]\n" << pad << '}';
}

std::string tool_label(EditorToolId tool) {
    switch (tool) {
        case EditorToolId::Select: return "Select";
        case EditorToolId::Translate: return "Move";
        case EditorToolId::AddVoxel: return "Add voxel";
        case EditorToolId::RemoveVoxel: return "Remove voxel";
        case EditorToolId::PaintMaterial: return "Paint material";
        case EditorToolId::Box: return "Box";
        case EditorToolId::Beam: return "Beam";
        case EditorToolId::Anchor: return "Anchor";
        case EditorToolId::Rotate: return "Rotate";
    }
    return "Tool";
}

} // namespace

AccessibilityNode build_editor_accessibility_tree(const NativeEditorController& controller) {
    AccessibilityNode root{"dve.editor", AccessibilityRole::Application,
                           "Destructible Voxel Engine Editor",
                           "Voxel scene authoring application", {}, true, false, false, {}};

    AccessibilityNode menuBar{"menuBar", AccessibilityRole::MenuBar, "Application menus", {}, {}, true,
                              controller.focus_region() == EditorFocusRegion::MenuBar, false, {}};
    for (std::string_view menuName : kMenuBarNames) {
        AccessibilityNode menu{"menu." + std::string(menuName), AccessibilityRole::Menu,
                               std::string(menuName), {}, {}, true, false, false, {}};
        for (const MenuAction& action : controller.menu_actions(menuName)) {
            menu.children.push_back({action.id, AccessibilityRole::MenuItem, action.label,
                                     action.menu + " command", action.shortcut, action.enabled,
                                     false, action.checked, {}});
        }
        menuBar.children.push_back(std::move(menu));
    }
    root.children.push_back(std::move(menuBar));

    AccessibilityNode toolbar{"toolbar", AccessibilityRole::ToolBar, "Authoring tools", {}, {}, true,
                              controller.focus_region() == EditorFocusRegion::Toolbar, false, {}};
    for (int value = 0; value <= static_cast<int>(EditorToolId::Rotate); ++value) {
        const auto tool = static_cast<EditorToolId>(value);
        toolbar.children.push_back({"tool." + std::to_string(value + 1), AccessibilityRole::Button,
                                    tool_label(tool), "Activate authoring tool", std::to_string(value + 1), true,
                                    false, controller.active_tool() == tool, {}});
    }
    root.children.push_back(std::move(toolbar));

    AccessibilityNode hierarchy{"sceneHierarchy", AccessibilityRole::Tree, "Scene hierarchy", {}, {}, true,
                                controller.focus_region() == EditorFocusRegion::SceneHierarchy, false, {}};
    for (EditorObjectId id : controller.hierarchy_order()) {
        const EditorObject* object = controller.workspace().document().find_object(id);
        if (!object) continue;
        std::string description = std::to_string(object->voxels->occupied_voxel_count()) + " voxels";
        if (object->flags.locked) description += ", locked";
        if (object->flags.anchored) description += ", anchored";
        hierarchy.children.push_back({"object." + std::to_string(id), AccessibilityRole::TreeItem,
                                      object->name, description, {}, true, false,
                                      controller.workspace().is_selected(id), {}});
    }
    root.children.push_back(std::move(hierarchy));

    AccessibilityNode viewport{"viewport", AccessibilityRole::Viewport, "3D hybrid viewport",
                               "Right mouse free-looks; Alt-left orbits; middle mouse pans; Mouse4 uses its active context binding",
                               {}, true, controller.focus_region() == EditorFocusRegion::Viewport, false, {}};
    root.children.push_back(std::move(viewport));

    AccessibilityNode inspector{"inspector", AccessibilityRole::Inspector, "Object inspector", {}, {}, true,
                                controller.focus_region() == EditorFocusRegion::Inspector, false, {}};
    if (controller.workspace().selected_object()) {
        const EditorObject* object = controller.workspace().document().find_object(*controller.workspace().selected_object());
        if (object) {
            const EditorSelectionDiagnostics diagnostics = controller.selection_diagnostics();
            inspector.description = object->name + ", " + std::to_string(controller.workspace().selection_count()) +
                                    " selected, mass " + std::to_string(diagnostics.massKilograms) + " kilograms";
        }
    } else inspector.description = "No selected object";
    root.children.push_back(std::move(inspector));

    if (controller.shortcut_panel().open) {
        AccessibilityNode shortcuts{"shortcutEditor", AccessibilityRole::Panel,
                                    "Keyboard and mouse shortcuts",
                                    "Profile " + std::string(controller.workspace().shortcuts().active_profile()),
                                    {}, true, true, false, {}};
        const auto rows = controller.shortcut_rows();
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const ShortcutSearchResult& row = rows[index];
            const std::string primary = row.bindings.primary ? format_shortcut_gesture(*row.bindings.primary) : std::string{};
            const std::string secondary = row.bindings.secondary ? format_shortcut_gesture(*row.bindings.secondary) : std::string{};
            shortcuts.children.push_back({"shortcut." + row.command->actionId + "." + shortcut_context_name(row.context),
                AccessibilityRole::Button, row.command->label,
                row.command->category + ", " + shortcut_context_name(row.context),
                secondary.empty() ? primary : primary + " or " + secondary,
                true, index == controller.shortcut_panel().selectedRow,
                index == controller.shortcut_panel().selectedRow, {}});
        }
        root.children.push_back(std::move(shortcuts));
    }

    root.children.push_back({"bottomPanel", AccessibilityRole::Panel, "Problems, tasks, console, and profiler",
                             std::to_string(controller.workspace().problems().error_count()) + " errors", {}, true,
                             controller.focus_region() == EditorFocusRegion::BottomPanel, false, {}});
    root.children.push_back({"status", AccessibilityRole::StatusText, "Status",
                             controller.status().text, {}, true, false, false, {}});
    return root;
}

std::string serialize_accessibility_tree_json(const AccessibilityNode& root) {
    std::ostringstream out;
    write_node(out, root, 0);
    out << '\n';
    return out.str();
}

bool save_accessibility_tree_json(
    const std::filesystem::path& path,
    const AccessibilityNode& root,
    std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { if (error) *error = "could not create accessibility output directory"; return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { if (error) *error = "could not open accessibility output"; return false; }
    output << serialize_accessibility_tree_json(root);
    if (!output) { if (error) *error = "could not write accessibility output"; return false; }
    return true;
}

} // namespace dve::editor
