#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dve::editor {

enum class SettingType : std::uint8_t { Boolean, Integer, Float, Enum, String };
enum class SettingScope : std::uint8_t { User, Project, Session };
enum class SettingApplyPolicy : std::uint8_t { Live, OnApply, RestartRequired };

enum SettingBuildCapability : std::uint32_t {
    SettingCapabilityNone = 0U,
    SettingCapabilityVoxel = 1U << 0U,
    SettingCapabilityPolygon = 1U << 1U,
    SettingCapabilityAudio = 1U << 2U,
    SettingCapabilityEditor = 1U << 3U,
    SettingCapabilityVulkan = 1U << 4U,
    SettingCapabilityLua = 1U << 5U,
    SettingCapabilityJolt = 1U << 6U,
    SettingCapabilityAll = 0xFFFFFFFFU,
};

using SettingValue = std::variant<bool, std::int64_t, double, std::string>;

struct SettingChoice {
    std::string value;
    std::string label;
};

struct SettingDependency {
    std::string settingId;
    SettingValue requiredValue{false};
    std::string explanation;
};

struct SettingAvailability {
    bool available{true};
    std::string explanation;
};

struct SettingDefinition {
    std::string id;
    std::string category;
    std::string section;
    std::string label;
    std::string description;
    SettingType type{SettingType::Boolean};
    SettingValue defaultValue{false};
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> step;
    std::vector<SettingChoice> choices;
    std::vector<std::string> keywords;
    SettingApplyPolicy applyPolicy{SettingApplyPolicy::Live};
    std::uint32_t requiredCapabilities{SettingCapabilityNone};
    bool advanced{};
    std::vector<SettingDependency> dependencies;
    // False while nothing in the engine or editor reads this setting yet, so changing
    // it has no effect. The Settings panel says so, and dve_settings_applied_tests
    // keeps the flag in sync with the code.
    bool applied{true};
};

struct SettingSearchResult {
    const SettingDefinition* definition{};
    SettingValue value{false};
    SettingScope sourceScope{SettingScope::User};
    bool inherited{true};
    int score{};
};

class EditorSettingsRegistry {
public:
    [[nodiscard]] bool add(SettingDefinition definition, std::string* error = nullptr);
    [[nodiscard]] const SettingDefinition* find(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<std::string> categories(std::uint32_t capabilities = SettingCapabilityAll) const;
    [[nodiscard]] std::vector<const SettingDefinition*> category(
        std::string_view categoryName,
        bool includeAdvanced = true,
        std::uint32_t capabilities = SettingCapabilityAll) const;
    [[nodiscard]] std::vector<SettingSearchResult> search(
        std::string_view query,
        bool includeAdvanced = true,
        std::uint32_t capabilities = SettingCapabilityAll,
        std::size_t limit = 64) const;

    [[nodiscard]] SettingValue value(std::string_view id) const;
    [[nodiscard]] SettingValue value(std::string_view id, SettingScope* sourceScope, bool* inherited = nullptr) const;
    [[nodiscard]] SettingValue inherited_value(std::string_view id, SettingScope excludingScope) const;
    [[nodiscard]] bool has_override(SettingScope scope, std::string_view id) const noexcept;
    [[nodiscard]] bool set(SettingScope scope, std::string_view id, SettingValue value,
                           std::string* error = nullptr);
    [[nodiscard]] bool clear(SettingScope scope, std::string_view id) noexcept;
    void clear_scope(SettingScope scope) noexcept;
    [[nodiscard]] bool differs_from_default(std::string_view id) const;
    [[nodiscard]] SettingAvailability availability(std::string_view id,
                                                   std::uint32_t capabilities = SettingCapabilityAll) const;
    [[nodiscard]] SettingAvailability choice_availability(std::string_view id, std::string_view choice) const;
    [[nodiscard]] bool set_dependencies(std::string_view id, std::vector<SettingDependency> dependencies,
                                        std::string* error = nullptr);
    [[nodiscard]] std::vector<SettingSearchResult> changed(
        bool includeAdvanced = true, std::uint32_t capabilities = SettingCapabilityAll) const;
    [[nodiscard]] std::size_t reset_category(SettingScope scope, std::string_view categoryName) noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;

    [[nodiscard]] std::string serialize_scope(SettingScope scope) const;
    // Atomic (write temp + rename) file persistence of one layer, e.g. User settings in
    // <project>/.dve/user/editor_settings.txt. load_scope_file leaves the layer untouched on failure.
    [[nodiscard]] bool save_scope_file(SettingScope scope, const std::filesystem::path& path,
                                       std::string* error = nullptr) const;
    [[nodiscard]] bool load_scope_file(SettingScope scope, const std::filesystem::path& path,
                                       std::string* error = nullptr);
    [[nodiscard]] bool parse_scope(SettingScope scope, std::string_view text,
                                   std::string* error = nullptr);
    [[nodiscard]] std::string serialize_profile(std::string_view profileName, SettingScope scope) const;
    [[nodiscard]] bool parse_profile(SettingScope scope, std::string_view text,
                                     std::string* error = nullptr);
    [[nodiscard]] const std::vector<std::string>& orphaned_settings() const noexcept { return orphanedSettings_; }

    [[nodiscard]] const std::vector<SettingDefinition>& definitions() const noexcept { return definitions_; }
    // Changes to layers (including loads, resets and profile imports) wake consumers once.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] static EditorSettingsRegistry make_default();

private:
    [[nodiscard]] bool validate_value(const SettingDefinition& definition, const SettingValue& value,
                                      std::string* error = nullptr) const;
    [[nodiscard]] std::map<std::string, SettingValue, std::less<>>& layer(SettingScope scope) noexcept;
    [[nodiscard]] const std::map<std::string, SettingValue, std::less<>>& layer(SettingScope scope) const noexcept;

    std::vector<SettingDefinition> definitions_;
    std::map<std::string, SettingValue, std::less<>> userValues_;
    std::map<std::string, SettingValue, std::less<>> projectValues_;
    std::map<std::string, SettingValue, std::less<>> sessionValues_;
    std::vector<std::string> orphanedSettings_;
    std::uint64_t revision_{};
};

[[nodiscard]] std::string setting_value_to_string(const SettingValue& value);
[[nodiscard]] std::string setting_scope_name(SettingScope scope);

struct EditorSettingsPanelState {
    bool open{};
    SettingScope scope{SettingScope::User};
    std::string selectedCategory{"Camera"};
    std::string searchQuery;
    std::size_t selectedRow{};
    bool includeAdvanced{};
    bool changedOnly{};
    bool dirty{};
    bool valueEditing{};
    std::string valueEditId;
    std::string valueEditBuffer;
    bool valueEditReplaceOnNextInput{true};
    std::string status;
    std::map<std::string, SettingValue, std::less<>> stagedValues;
    std::set<std::string, std::less<>> stagedClears;
    // Choices supplied at runtime for String settings whose options depend on the machine
    // (e.g. `midi.input_port` lists the MIDI ports that are present). Such settings cycle through
    // these choices with left/right/Enter/click instead of opening a text edit, and the row shows
    // the choice label. The stored value may still be something that is not listed right now
    // (an unplugged device), so the registry keeps validating them as plain strings.
    std::map<std::string, std::vector<SettingChoice>, std::less<>> dynamicChoices;

    [[nodiscard]] bool has_choices(const SettingDefinition& definition) const;
    [[nodiscard]] std::string value_label(const SettingDefinition& definition, const SettingValue& value) const;

    void open_for(SettingScope newScope, std::string category = {});
    void close() noexcept;
    void discard() noexcept;
    [[nodiscard]] SettingValue displayed_value(const EditorSettingsRegistry& registry,
                                               std::string_view id) const;
    [[nodiscard]] bool stage(const EditorSettingsRegistry& registry, std::string_view id,
                             SettingValue value, std::string* error = nullptr);
    [[nodiscard]] bool cycle(const EditorSettingsRegistry& registry, std::string_view id, int direction,
                             std::string* error = nullptr);
    [[nodiscard]] bool begin_value_edit(const EditorSettingsRegistry& registry, std::string_view id,
                                        std::string* error = nullptr);
    void append_value_text(std::string_view text);
    void backspace_value_text();
    void cancel_value_edit() noexcept;
    [[nodiscard]] bool commit_value_edit(const EditorSettingsRegistry& registry,
                                         std::string* error = nullptr);
    [[nodiscard]] bool reset(const EditorSettingsRegistry& registry, std::string_view id,
                             std::string* error = nullptr);
    [[nodiscard]] std::size_t reset_category(const EditorSettingsRegistry& registry,
                                             std::string_view categoryName);
    [[nodiscard]] bool apply(EditorSettingsRegistry& registry, std::string* error = nullptr);
};

} // namespace dve::editor
