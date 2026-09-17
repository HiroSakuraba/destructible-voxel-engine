#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/editor_settings.hpp"
#include "dve/editor_shortcuts.hpp"
#include "dve/editor_ai_assistant.hpp"

namespace dve::editor {

enum class EditorMode : std::uint8_t { Edit, Simulate, Play };
enum class PanelId : std::uint8_t {
    Project, Assets, SceneHierarchy, Viewport, Inspector,
    Problems, Tasks, Console, Profiler, Build, Assistant
};

struct EditorPreferences {
    float uiScale{1.0F};
    float cameraSpeed{5.0F};
    float mouseSensitivity{1.0F};
    std::uint32_t autosaveMinutes{5};
    float translateSnapMeters{0.10F};
    float rotateSnapDegrees{15.0F};
    bool highContrast{};
    bool reducedMotion{};
    bool colorBlindSafeDiagnostics{true};
    bool confirmDestructiveActions{true};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<EditorPreferences> parse(std::string_view text,
                                                                 std::string* error = nullptr);
};

enum class MenuVisibility : std::uint8_t { Primary, Advanced, PaletteOnly };

struct MenuAction {
    MenuAction() = default;
    MenuAction(std::string actionId, std::string menuName, std::string actionLabel,
               std::string actionShortcut = {})
        : id(std::move(actionId)), menu(std::move(menuName)), label(std::move(actionLabel)),
          shortcut(std::move(actionShortcut)) {}

    std::string id;
    std::string menu;
    std::string label;
    std::string shortcut;
    std::string description;
    std::string disabledReason;
    bool enabled{true};
    bool checked{};
    std::string section;
    std::int32_t order{};
    bool checkable{};
    std::string radioGroup;
    std::vector<std::string> keywords;
    MenuVisibility visibility{MenuVisibility::Primary};
    bool dangerous{};
};

class EditorMenuRegistry {
public:
    [[nodiscard]] bool add(MenuAction action, std::string* error = nullptr);
    [[nodiscard]] const MenuAction* find(std::string_view id) const noexcept;
    [[nodiscard]] MenuAction* find(std::string_view id) noexcept;
    [[nodiscard]] bool set_enabled(std::string_view id, bool enabled, std::string reason = {}) noexcept;
    [[nodiscard]] bool set_checked(std::string_view id, bool checked) noexcept;
    [[nodiscard]] bool set_shortcut(std::string_view id, std::string shortcut) noexcept;
    [[nodiscard]] std::vector<MenuAction> menu(std::string_view menuName, bool includeAdvanced = false) const;
    [[nodiscard]] std::vector<MenuAction> search(std::string_view query, std::size_t limit = 12) const;
    [[nodiscard]] const std::vector<MenuAction>& actions() const noexcept { return actions_; }
    [[nodiscard]] static EditorMenuRegistry make_default();
private:
    std::vector<MenuAction> actions_;
};

struct EditorMenuUserState {
    bool showAdvancedCommands{};
    std::vector<std::string> favoriteActionIds;
    std::vector<std::string> recentActionIds;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<EditorMenuUserState> parse(
        std::string_view text, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] static std::optional<EditorMenuUserState> load(
        const std::filesystem::path& path, std::string* error = nullptr);
};

enum class ProblemSeverity : std::uint8_t { Information, Warning, Error };
struct EditorProblem {
    ProblemSeverity severity{ProblemSeverity::Information};
    std::string code;
    std::string summary;
    std::string details;
    std::optional<EditorObjectId> objectId;
    std::vector<std::string> suggestedActions;
};

class EditorProblemStore {
public:
    void add(EditorProblem problem);
    void clear() noexcept { problems_.clear(); }
    void clear_code(std::string_view code);
    [[nodiscard]] std::size_t error_count() const noexcept;
    [[nodiscard]] const std::vector<EditorProblem>& problems() const noexcept { return problems_; }
private:
    std::vector<EditorProblem> problems_;
};

enum class EditorLogLevel : std::uint8_t { Info, Warning, Error };

struct EditorLogEntry {
    EditorLogLevel level{EditorLogLevel::Info};
    std::string text;
};

// Backs the Console panel. Status messages (NativeEditorController::set_status) are
// transient by design, disappearing after a few seconds so they don't permanently occupy
// screen space; the log keeps every one of them (bounded) so a message that already scrolled
// off the status bar is still visible somewhere. Not a general logging facility for the rest
// of the engine, just what the editor itself has told the user.
class EditorLogStore {
public:
    explicit EditorLogStore(std::size_t capacity = 200) : capacity_(capacity) {}
    void add(EditorLogLevel level, std::string text);
    void clear() noexcept { entries_.clear(); }
    [[nodiscard]] const std::deque<EditorLogEntry>& entries() const noexcept { return entries_; }
private:
    std::deque<EditorLogEntry> entries_;
    std::size_t capacity_;
};

class EditorWorkspace {
public:
    explicit EditorWorkspace(EditorDocument document = EditorDocument{});
    [[nodiscard]] EditorDocument& document() noexcept { return document_; }
    [[nodiscard]] const EditorDocument& document() const noexcept { return document_; }
    [[nodiscard]] EditorCommandStack& commands() noexcept { return commands_; }
    [[nodiscard]] const EditorCommandStack& commands() const noexcept { return commands_; }
    [[nodiscard]] EditorMenuRegistry& menus() noexcept { return menus_; }
    [[nodiscard]] const EditorMenuRegistry& menus() const noexcept { return menus_; }
    [[nodiscard]] EditorProblemStore& problems() noexcept { return problems_; }
    [[nodiscard]] const EditorProblemStore& problems() const noexcept { return problems_; }
    [[nodiscard]] EditorLogStore& log() noexcept { return log_; }
    [[nodiscard]] const EditorLogStore& log() const noexcept { return log_; }
    [[nodiscard]] EditorPreferences& preferences() noexcept { return preferences_; }
    [[nodiscard]] const EditorPreferences& preferences() const noexcept { return preferences_; }
    [[nodiscard]] EditorSettingsRegistry& settings() noexcept { return settings_; }
    [[nodiscard]] const EditorSettingsRegistry& settings() const noexcept { return settings_; }
    [[nodiscard]] EditorShortcutRegistry& shortcuts() noexcept { return shortcuts_; }
    [[nodiscard]] EditorAiAssistantPanel& ai_assistant() noexcept { return aiAssistant_; }
    [[nodiscard]] const EditorAiAssistantPanel& ai_assistant() const noexcept { return aiAssistant_; }
    [[nodiscard]] const EditorShortcutRegistry& shortcuts() const noexcept { return shortcuts_; }
    void synchronize_preferences_from_settings() noexcept;
    [[nodiscard]] EditorMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool set_mode(EditorMode mode, std::string* error = nullptr);
    void set_panel_visible(PanelId panel, bool visible);
    [[nodiscard]] bool panel_visible(PanelId panel) const;
    void select_object(std::optional<EditorObjectId> id) noexcept;
    void add_to_selection(EditorObjectId id) noexcept;
    void toggle_selection(EditorObjectId id) noexcept;
    void clear_selection() noexcept;
    void prune_selection() noexcept;
    [[nodiscard]] bool is_selected(EditorObjectId id) const noexcept { return selection_.contains(id); }
    [[nodiscard]] std::optional<EditorObjectId> selected_object() const noexcept { return primarySelection_; }
    [[nodiscard]] const std::set<EditorObjectId>& selected_objects() const noexcept { return selection_; }
    [[nodiscard]] std::size_t selection_count() const noexcept { return selection_.size(); }
    [[nodiscard]] bool restore_pre_simulation(std::string* error = nullptr);
    [[nodiscard]] bool accept_simulation_state(std::string* error = nullptr);
    [[nodiscard]] bool has_pre_simulation_snapshot() const noexcept { return preSimulationSnapshot_.has_value(); }
private:
    EditorDocument document_;
    std::optional<EditorDocument> preSimulationSnapshot_;
    std::set<EditorObjectId> preSimulationSelection_;
    std::optional<EditorObjectId> preSimulationPrimarySelection_;
    EditorCommandStack commands_;
    EditorMenuRegistry menus_;
    EditorProblemStore problems_;
    EditorLogStore log_;
    EditorPreferences preferences_;
    EditorSettingsRegistry settings_;
    EditorShortcutRegistry shortcuts_;
    EditorAiAssistantPanel aiAssistant_;
    std::map<PanelId, bool> panels_;
    std::set<EditorObjectId> selection_;
    std::optional<EditorObjectId> primarySelection_;
    EditorMode mode_{EditorMode::Edit};
};

} // namespace dve::editor
