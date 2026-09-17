#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve::editor {

enum class ShortcutContext : std::uint8_t {
    Global,
    Viewport,
    FlyNavigation,
    PlayMode,
    Camera,
    Sequencer,
    AudioEditor,
    VoxelEditor,
    PolygonEditor,
    MaterialEditor,
    AssetBrowser,
    TextField,
    ModalDialog,
};

enum class ShortcutDevice : std::uint8_t { Keyboard, MouseButton, Wheel };
enum class ShortcutActivation : std::uint8_t { Press, Hold, DoubleClick };
enum class ShortcutSlot : std::uint8_t { Primary, Secondary };

struct ShortcutGesture {
    ShortcutDevice device{ShortcutDevice::Keyboard};
    ShortcutActivation activation{ShortcutActivation::Press};
    std::string input;
    bool control{};
    bool shift{};
    bool alt{};

    [[nodiscard]] bool operator==(const ShortcutGesture&) const noexcept = default;
    [[nodiscard]] bool valid() const noexcept { return !input.empty(); }
};

struct ShortcutCommandDefinition {
    std::string actionId;
    std::string label;
    std::string category;
    std::vector<ShortcutContext> contexts{ShortcutContext::Global};
    std::uint32_t requiredCapabilities{};
    bool repeatable{};
    bool destructive{};
    std::vector<std::string> keywords;
};

struct ShortcutBindingPair {
    std::optional<ShortcutGesture> primary;
    std::optional<ShortcutGesture> secondary;
};

struct ShortcutResolution {
    std::string actionId;
    ShortcutContext context{ShortcutContext::Global};
    ShortcutSlot slot{ShortcutSlot::Primary};
};

struct ShortcutConflict {
    std::string actionId;
    std::string otherActionId;
    ShortcutContext context{ShortcutContext::Global};
    ShortcutGesture gesture;
    std::string explanation;
};

struct ShortcutSearchResult {
    const ShortcutCommandDefinition* command{};
    ShortcutBindingPair bindings;
    ShortcutContext context{ShortcutContext::Global};
    int score{};
};

struct ShortcutKeyboardKeyVisual {
    std::string key;
    std::vector<std::string> actions;
    bool conflicted{};
};

class EditorShortcutRegistry {
public:
    [[nodiscard]] bool add_command(ShortcutCommandDefinition command, std::string* error = nullptr);
    [[nodiscard]] const ShortcutCommandDefinition* find_command(std::string_view actionId) const noexcept;
    [[nodiscard]] bool has_profile(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string> profile_names() const;
    [[nodiscard]] std::string_view active_profile() const noexcept { return activeProfile_; }
    [[nodiscard]] bool set_active_profile(std::string_view name, std::string* error = nullptr);
    [[nodiscard]] bool duplicate_profile(std::string_view source, std::string_view destination,
                                         std::string* error = nullptr);
    [[nodiscard]] bool remove_profile(std::string_view name, std::string* error = nullptr);
    [[nodiscard]] bool reset_profile(std::string_view name, std::string* error = nullptr);

    [[nodiscard]] bool set_binding(std::string_view profile, std::string_view actionId,
                                   ShortcutContext context, ShortcutSlot slot,
                                   std::optional<ShortcutGesture> gesture,
                                   bool overrideConflicts = false,
                                   std::string* error = nullptr);
    [[nodiscard]] ShortcutBindingPair bindings(std::string_view profile, std::string_view actionId,
                                                ShortcutContext context) const;
    [[nodiscard]] ShortcutBindingPair bindings(std::string_view actionId,
                                                ShortcutContext context) const {
        return bindings(activeProfile_, actionId, context);
    }
    [[nodiscard]] std::optional<ShortcutResolution> resolve(
        const ShortcutGesture& gesture,
        std::span<const ShortcutContext> activeContexts,
        std::uint32_t capabilities = 0xFFFFFFFFU) const;
    [[nodiscard]] std::vector<ShortcutConflict> conflicts(std::string_view profile) const;
    [[nodiscard]] std::vector<ShortcutSearchResult> search(
        std::string_view query,
        std::optional<ShortcutContext> context = std::nullopt,
        std::uint32_t capabilities = 0xFFFFFFFFU,
        std::size_t limit = 128) const;
    [[nodiscard]] std::vector<ShortcutKeyboardKeyVisual> keyboard_map(
        std::optional<ShortcutContext> context = std::nullopt) const;
    [[nodiscard]] std::string display_binding(
        std::string_view actionId,
        std::span<const ShortcutContext> preferredContexts = {}) const;

    [[nodiscard]] std::string serialize_profile(std::string_view profile) const;
    [[nodiscard]] bool parse_profile(std::string_view text, std::string* error = nullptr);

    void install_builtin_commands();
    void install_builtin_profiles();

    [[nodiscard]] const std::vector<ShortcutCommandDefinition>& commands() const noexcept {
        return commands_;
    }

private:
    struct BindingKey {
        std::string actionId;
        ShortcutContext context{ShortcutContext::Global};
        [[nodiscard]] bool operator<(const BindingKey& other) const noexcept {
            if (actionId != other.actionId) return actionId < other.actionId;
            return context < other.context;
        }
    };
    using ProfileBindings = std::map<BindingKey, ShortcutBindingPair>;

    [[nodiscard]] bool gesture_conflicts(std::string_view profile, std::string_view actionId,
                                         ShortcutContext context, const ShortcutGesture& gesture,
                                         std::vector<ShortcutConflict>* conflictsOut = nullptr) const;
    [[nodiscard]] bool contexts_overlap(ShortcutContext a, ShortcutContext b) const noexcept;

    std::vector<ShortcutCommandDefinition> commands_;
    std::map<std::string, ProfileBindings, std::less<>> profiles_;
    std::map<std::string, ProfileBindings, std::less<>> builtinDefaults_;
    std::string activeProfile_{"DVE Default"};
};

[[nodiscard]] std::string shortcut_context_name(ShortcutContext context);
[[nodiscard]] std::optional<ShortcutContext> parse_shortcut_context(std::string_view text);
[[nodiscard]] std::string format_shortcut_gesture(const ShortcutGesture& gesture);
[[nodiscard]] std::optional<ShortcutGesture> parse_shortcut_gesture(std::string_view text,
                                                                     std::string* error = nullptr);
[[nodiscard]] ShortcutGesture keyboard_shortcut(std::string key, bool control = false,
                                                 bool shift = false, bool alt = false,
                                                 ShortcutActivation activation = ShortcutActivation::Press);
[[nodiscard]] ShortcutGesture mouse_shortcut(std::string button, bool control = false,
                                              bool shift = false, bool alt = false,
                                              ShortcutActivation activation = ShortcutActivation::Press);
[[nodiscard]] ShortcutGesture wheel_shortcut(bool up, bool control = false,
                                              bool shift = false, bool alt = false);

} // namespace dve::editor
