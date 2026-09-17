#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dve/transform.hpp"

namespace dve::ui {

enum class GameMenuScreen : std::uint8_t {
    Closed, Title, Pause, Settings, Graphics, Audio, Controls,
    Accessibility, SaveLoad, MissionSelect, QuitConfirmation
};

struct GameSettings {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t frameRateLimit{120};
    std::uint64_t preferredDisplayId{};
    std::uint32_t localPlayerCount{1};
    float renderScale{1.0F};
    float masterVolume{1.0F};
    float effectsVolume{1.0F};
    float musicVolume{0.8F};
    float fieldOfViewDegrees{90.0F};
    float mouseSensitivity{1.0F};
    float uiScale{1.0F};
    float screenShake{1.0F};
    bool fullscreen{}; // legacy compatibility mirror of displayMode
    bool highDpi{true};
    bool spectatorWindow{};
    bool verticalSync{};
    bool invertY{};
    bool subtitles{true};
    bool reducedMotion{};
    bool holdToInteract{true};
    std::string colorVisionMode{"normal"};
    std::string displayMode{"windowed"};
    std::string resolutionProfile{"fhd_1080p"};
    std::string splitScreenLayout{"auto"};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<GameSettings> parse(std::string_view text,
                                                            std::string* error = nullptr);
};

class InputBindingMap {
public:
    [[nodiscard]] bool bind(std::string action, std::string device, std::string control,
                            std::string* error = nullptr);
    [[nodiscard]] bool unbind(std::string_view action, std::string_view device);
    [[nodiscard]] std::optional<std::string> binding(std::string_view action,
                                                      std::string_view device) const;
    [[nodiscard]] const std::map<std::string, std::map<std::string, std::string>>& all() const noexcept {
        return bindings_;
    }
private:
    std::map<std::string, std::map<std::string, std::string>> bindings_;
};

struct ToolWheelEntry {
    std::string id;
    std::string label;
    bool enabled{true};
};

class GameMenuModel {
public:
    void open(GameMenuScreen screen) noexcept { current_ = screen; }
    void close() noexcept { current_ = GameMenuScreen::Closed; }
    [[nodiscard]] GameMenuScreen current() const noexcept { return current_; }
    [[nodiscard]] std::vector<std::string> actions() const;
private:
    GameMenuScreen current_{GameMenuScreen::Title};
};

class GameHudModel {
public:
    void set_interaction_prompt(std::string text) { interactionPrompt_ = std::move(text); }
    [[nodiscard]] const std::string& interaction_prompt() const noexcept { return interactionPrompt_; }
    void set_tools(std::vector<ToolWheelEntry> tools);
    [[nodiscard]] bool select_next_tool();
    [[nodiscard]] std::optional<ToolWheelEntry> selected_tool() const;
private:
    std::string interactionPrompt_;
    std::vector<ToolWheelEntry> tools_;
    std::size_t selected_{};
};

using UiWidgetId = std::uint64_t;
inline constexpr UiWidgetId kInvalidUiWidgetId = 0U;
using UiCanvasId = std::uint64_t;
inline constexpr UiCanvasId kInvalidUiCanvasId = 0U;
using UiValue = std::variant<bool, std::int64_t, double, std::string>;

struct UiVec2 { float x{}; float y{}; };
struct UiRect { float x{}; float y{}; float width{}; float height{}; };
struct UiColor { float r{1.0F}; float g{1.0F}; float b{1.0F}; float a{1.0F}; };
struct UiEdges { float left{}; float top{}; float right{}; float bottom{}; };

enum class UiWidgetKind : std::uint8_t {
    CanvasRoot, Panel, Text, Image, Button, Slider, ProgressBar, List, Spacer, TextInput
};
enum class UiLayoutDirection : std::uint8_t { Absolute, Horizontal, Vertical, Overlay };
enum class UiCanvasMode : std::uint8_t { ScreenSpace, WorldSpace };
enum class UiBindingTarget : std::uint8_t { NoBinding, Text, Value, Visible, Enabled };
enum class UiNavigation : std::uint8_t { Next, Previous, Left, Right, Up, Down, Activate };

struct UiLayoutStyle {
    UiLayoutDirection direction{UiLayoutDirection::Absolute};
    UiVec2 position{};
    UiVec2 preferredSize{100.0F, 32.0F};
    UiVec2 minimumSize{};
    UiVec2 maximumSize{100000.0F, 100000.0F};
    UiEdges margin{};
    UiEdges padding{};
    float spacing{};
    float flexGrow{};
};

struct UiWidget {
    UiWidgetId id{kInvalidUiWidgetId};
    UiWidgetId parent{kInvalidUiWidgetId};
    std::vector<UiWidgetId> children;
    UiWidgetKind kind{UiWidgetKind::Panel};
    std::string name;
    std::string text;
    std::string localizationKey;
    std::string accessibilityLabel;
    std::string imageAsset;
    std::string binding;
    UiBindingTarget bindingTarget{UiBindingTarget::NoBinding};
    UiLayoutStyle layout{};
    UiColor color{};
    double value{};
    double minimum{};
    double maximum{1.0};
    double step{0.1};
    bool visible{true};
    bool enabled{true};
    bool focusable{};
    bool modal{};
};

struct UiCanvasDesc {
    std::string name;
    UiCanvasMode mode{UiCanvasMode::ScreenSpace};
    UiVec2 logicalSize{1920.0F, 1080.0F};
    RigidTransform worldTransform{};
    float metersPerLogicalUnit{0.001F};
    std::int32_t layer{};
    bool visible{true};
};

struct UiDrawCommand {
    UiCanvasId canvas{kInvalidUiCanvasId};
    UiWidgetId widget{kInvalidUiWidgetId};
    UiWidgetKind kind{UiWidgetKind::Panel};
    UiRect rectangle{};
    UiRect clipRectangle{};
    UiColor color{};
    std::string text;
    std::string imageAsset;
    std::string localizationKey;
    double value{};
    UiCanvasMode canvasMode{UiCanvasMode::ScreenSpace};
    RigidTransform worldTransform{};
    std::int32_t layer{};
    float textScale{1.0F};
    bool enabled{true};
    bool focused{};
};

enum class UiRenderPrimitiveKind : std::uint8_t {
    Quad, Text, Image, ProgressFill, SliderTrack, SliderThumb, TextCaret, FocusRing
};

struct UiRenderPrimitive {
    UiCanvasId canvas{kInvalidUiCanvasId};
    UiWidgetId widget{kInvalidUiWidgetId};
    UiRenderPrimitiveKind kind{UiRenderPrimitiveKind::Quad};
    UiRect rectangle{};
    UiRect clipRectangle{};
    UiColor color{};
    std::string text;
    std::string imageAsset;
    UiCanvasMode canvasMode{UiCanvasMode::ScreenSpace};
    RigidTransform worldTransform{};
    std::int32_t layer{};
    float textScale{1.0F};
};

struct UiEvent {
    UiCanvasId canvas{kInvalidUiCanvasId};
    UiWidgetId widget{kInvalidUiWidgetId};
    std::string action;
    UiValue value{false};
};

class UiDataModel {
public:
    void set(std::string key, UiValue value);
    [[nodiscard]] const UiValue* get(std::string_view key) const noexcept;
private:
    std::map<std::string, UiValue, std::less<>> values_;
};

struct UiLocalizationTable {
    std::string locale;
    std::string fallbackLocale;
    std::map<std::string, std::string, std::less<>> messages;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

class UiLocalizer {
public:
    [[nodiscard]] bool register_table(UiLocalizationTable table, std::string* error = nullptr);
    [[nodiscard]] bool set_locale(std::string locale, std::string* error = nullptr);
    [[nodiscard]] const std::string& locale() const noexcept { return locale_; }
    void set_pseudo_localization(bool enabled) noexcept { pseudoLocalization_ = enabled; }
    [[nodiscard]] bool pseudo_localization() const noexcept { return pseudoLocalization_; }
    [[nodiscard]] std::string resolve(std::string_view key, const UiDataModel& data) const;
    [[nodiscard]] const std::vector<std::string>& missing_keys() const noexcept { return missingKeys_; }
    void clear_missing_keys() const { missingKeys_.clear(); }
private:
    std::map<std::string, UiLocalizationTable, std::less<>> tables_;
    std::string locale_{"en"};
    bool pseudoLocalization_{};
    mutable std::vector<std::string> missingKeys_;
};

class UiDocument {
public:
    UiDocument();
    [[nodiscard]] UiWidgetId root() const noexcept { return root_; }
    [[nodiscard]] UiWidgetId add_widget(
        UiWidgetId parent, UiWidgetKind kind, std::string name,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_widget(UiWidgetId id);
    [[nodiscard]] UiWidget* widget(UiWidgetId id) noexcept;
    [[nodiscard]] const UiWidget* widget(UiWidgetId id) const noexcept;
    [[nodiscard]] UiWidgetId find_widget(std::string_view name) const noexcept;
    [[nodiscard]] const std::map<UiWidgetId, UiWidget>& widgets() const noexcept { return widgets_; }
    [[nodiscard]] UiWidgetId next_id() const noexcept { return nextId_; }
    [[nodiscard]] bool replace_records(
        UiWidgetId root, UiWidgetId nextId, std::map<UiWidgetId, UiWidget> widgets,
        std::string* error = nullptr);
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::vector<UiDrawCommand> build_draw_list(
        UiCanvasId canvasId, const UiCanvasDesc& canvas, const UiDataModel& data,
        UiWidgetId focused = kInvalidUiWidgetId, std::string* error = nullptr) const;
    [[nodiscard]] UiWidgetId first_focusable(const UiDataModel* data = nullptr) const noexcept;
    [[nodiscard]] UiWidgetId next_focusable(
        UiWidgetId current, bool reverse = false, const UiDataModel* data = nullptr,
        UiWidgetId scopeRoot = kInvalidUiWidgetId) const noexcept;
    [[nodiscard]] bool is_descendant(UiWidgetId widget, UiWidgetId ancestor) const noexcept;
private:
    UiWidgetId root_{1U};
    UiWidgetId nextId_{2U};
    std::map<UiWidgetId, UiWidget> widgets_;
};

struct UiAccessibilityStyle {
    float textScale{1.0F};
    bool highContrast{};
    bool reducedMotion{};
    bool screenReaderHints{};
};

struct UiAsset {
    std::string name;
    UiCanvasDesc canvas;
    UiDocument document;
    std::uint64_t contentHash{};
};

struct UiAssetReadResult {
    UiAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] bool validate_ui_asset(const UiAsset& asset, std::string* error = nullptr);
[[nodiscard]] std::uint64_t ui_asset_content_hash(const UiAsset& asset) noexcept;
[[nodiscard]] bool write_dveui(
    const std::filesystem::path& path, const UiAsset& asset, std::string* error = nullptr);
[[nodiscard]] UiAssetReadResult read_dveui(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 16ULL * 1024ULL * 1024ULL);

class UiRuntime {
public:
    [[nodiscard]] UiCanvasId create_canvas(UiCanvasDesc desc, std::string* error = nullptr);
    [[nodiscard]] UiCanvasId load_asset(
        const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool reload_asset(UiCanvasId id, std::string* error = nullptr);
    [[nodiscard]] std::size_t reload_changed_assets();
    [[nodiscard]] bool destroy_canvas(UiCanvasId id);
    [[nodiscard]] UiDocument* document(UiCanvasId id) noexcept;
    [[nodiscard]] const UiDocument* document(UiCanvasId id) const noexcept;
    [[nodiscard]] UiCanvasDesc* canvas(UiCanvasId id) noexcept;
    [[nodiscard]] const UiCanvasDesc* canvas(UiCanvasId id) const noexcept;
    [[nodiscard]] bool navigate(UiCanvasId id, UiNavigation navigation);
    [[nodiscard]] bool pointer_move(UiCanvasId id, float x, float y);
    [[nodiscard]] bool pointer_down(UiCanvasId id, float x, float y);
    [[nodiscard]] bool pointer_up(UiCanvasId id, float x, float y);
    [[nodiscard]] bool key_down(UiCanvasId id, std::string_view key, bool shift = false);
    [[nodiscard]] bool action(UiCanvasId id, std::string_view actionName);
    [[nodiscard]] bool text_input(UiCanvasId id, std::string_view text);
    [[nodiscard]] bool set_modal_root(UiCanvasId id, UiWidgetId root);
    [[nodiscard]] UiWidgetId hovered_widget(UiCanvasId id) const noexcept;
    [[nodiscard]] UiWidgetId captured_widget(UiCanvasId id) const noexcept;
    [[nodiscard]] std::vector<UiEvent> take_events();
    [[nodiscard]] std::vector<UiEvent> take_accessibility_events();
    void rebuild();
    [[nodiscard]] const std::vector<UiDrawCommand>& draw_commands() const noexcept { return drawCommands_; }
    [[nodiscard]] const std::vector<UiRenderPrimitive>& render_primitives() const noexcept {
        return renderPrimitives_;
    }
    [[nodiscard]] UiWidgetId focused_widget(UiCanvasId id) const noexcept;
    [[nodiscard]] UiDataModel& data() noexcept { return data_; }
    [[nodiscard]] const UiDataModel& data() const noexcept { return data_; }
    [[nodiscard]] UiLocalizer& localizer() noexcept { return localizer_; }
    [[nodiscard]] const UiLocalizer& localizer() const noexcept { return localizer_; }
    void set_accessibility(UiAccessibilityStyle style) noexcept;
    [[nodiscard]] const UiAccessibilityStyle& accessibility() const noexcept { return accessibility_; }
private:
    struct CanvasRecord {
        UiCanvasDesc desc;
        UiDocument document;
        UiWidgetId focused{};
        UiWidgetId hovered{};
        UiWidgetId captured{};
        UiWidgetId modalRoot{};
        std::map<UiWidgetId, std::size_t> textCaretBytes;
        std::filesystem::path sourcePath;
        std::filesystem::file_time_type sourceWriteTime{};
        std::uint64_t sourceHash{};
    };
    [[nodiscard]] UiWidgetId hit_test(const CanvasRecord& record, UiCanvasId id, float x, float y) const;
    void emit_focus_event(UiCanvasId id, UiWidgetId widget);
    void build_render_primitives();
    std::map<UiCanvasId, CanvasRecord> canvases_;
    UiCanvasId nextCanvasId_{1U};
    UiDataModel data_;
    UiLocalizer localizer_;
    UiAccessibilityStyle accessibility_{};
    std::vector<UiDrawCommand> drawCommands_;
    std::vector<UiRenderPrimitive> renderPrimitives_;
    std::vector<UiEvent> events_;
    std::vector<UiEvent> accessibilityEvents_;
};

[[nodiscard]] UiDocument make_default_gameplay_hud();

} // namespace dve::ui
