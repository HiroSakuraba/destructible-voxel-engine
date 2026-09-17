#include "dve/game_ui.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <sstream>
#include <functional>
#include <iterator>
#include <limits>
#include <set>

namespace dve::ui {
namespace {

std::map<std::string, std::string, std::less<>> parse_lines(std::string_view text) {
    std::map<std::string, std::string, std::less<>> result;
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto split = line.find('=');
        if (split != std::string::npos) result[line.substr(0, split)] = line.substr(split + 1);
    }
    return result;
}
template<class T> bool parse_number(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool interactive_widget(const UiWidget& widget) noexcept {
    return widget.focusable || widget.kind == UiWidgetKind::Button ||
        widget.kind == UiWidgetKind::Slider || widget.kind == UiWidgetKind::TextInput ||
        widget.kind == UiWidgetKind::List;
}

std::size_t previous_utf8_boundary(std::string_view text, std::size_t offset) noexcept {
    offset = std::min(offset, text.size());
    if (offset == 0U) return 0U;
    --offset;
    while (offset > 0U &&
           (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) --offset;
    return offset;
}

std::size_t next_utf8_boundary(std::string_view text, std::size_t offset) noexcept {
    offset = std::min(offset, text.size());
    if (offset >= text.size()) return text.size();
    ++offset;
    while (offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) ++offset;
    return offset;
}

std::size_t utf8_codepoint_count(std::string_view text, std::size_t byteLimit) noexcept {
    byteLimit = std::min(byteLimit, text.size());
    std::size_t count{};
    for (std::size_t i = 0U; i < byteLimit; i = next_utf8_boundary(text, i)) ++count;
    return count;
}

std::size_t utf8_boundary_for_codepoint(std::string_view text, std::size_t index) noexcept {
    std::size_t offset{};
    for (std::size_t i = 0U; i < index && offset < text.size(); ++i)
        offset = next_utf8_boundary(text, offset);
    return offset;
}

std::string filter_single_line_text(std::string_view text) {
    std::string filtered;
    filtered.reserve(text.size());
    for (unsigned char value : text) {
        if (value == '\n' || value == '\r' || value == '\t') filtered.push_back(' ');
        else if (value >= 32U) filtered.push_back(static_cast<char>(value));
    }
    return filtered;
}

} // namespace

bool GameSettings::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (width < 640 || height < 480 || width > 16384 || height > 16384) return fail("display resolution is outside supported limits");
    if (frameRateLimit < 30 || frameRateLimit > 1000) return fail("frame-rate limit is invalid");
    if (localPlayerCount == 0U || localPlayerCount > 4U) return fail("local player count is invalid");
    if (!std::isfinite(renderScale) || renderScale < 0.25F || renderScale > 2.0F) return fail("render scale is invalid");
    auto unit = [&](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; };
    if (!unit(masterVolume) || !unit(effectsVolume) || !unit(musicVolume) || !unit(screenShake)) return fail("volume or screen-shake value is invalid");
    if (!std::isfinite(fieldOfViewDegrees) || fieldOfViewDegrees < 60.0F || fieldOfViewDegrees > 130.0F) return fail("field of view is invalid");
    if (!std::isfinite(mouseSensitivity) || mouseSensitivity <= 0.0F || mouseSensitivity > 20.0F) return fail("mouse sensitivity is invalid");
    if (!std::isfinite(uiScale) || uiScale < 0.75F || uiScale > 3.0F) return fail("UI scale is invalid");
    static const std::vector<std::string> modes{"normal","deuteranopia","protanopia","tritanopia","high-contrast"};
    if (std::find(modes.begin(), modes.end(), colorVisionMode) == modes.end()) return fail("unknown color-vision mode");
    static const std::vector<std::string> displayModes{"windowed","borderless","exclusive"};
    if (std::find(displayModes.begin(), displayModes.end(), displayMode) == displayModes.end()) return fail("unknown display mode");
    static const std::vector<std::string> splitLayouts{"auto","single","horizontal","vertical","three-left","three-top","quad","picture-in-picture"};
    if (std::find(splitLayouts.begin(), splitLayouts.end(), splitScreenLayout) == splitLayouts.end()) return fail("unknown split-screen layout");
    if (resolutionProfile.empty() || resolutionProfile.size() > 64U) return fail("resolution profile is invalid");
    return true;
}
std::string GameSettings::serialize() const {
    std::ostringstream out;
    out << "DVE_GAME_SETTINGS=2\n" << "width=" << width << "\nheight=" << height
        << "\nframeRateLimit=" << frameRateLimit << "\npreferredDisplayId=" << preferredDisplayId
        << "\nlocalPlayerCount=" << localPlayerCount << "\nrenderScale=" << renderScale
        << "\nmasterVolume=" << masterVolume << "\neffectsVolume=" << effectsVolume
        << "\nmusicVolume=" << musicVolume << "\nfieldOfViewDegrees=" << fieldOfViewDegrees
        << "\nmouseSensitivity=" << mouseSensitivity << "\nuiScale=" << uiScale
        << "\nscreenShake=" << screenShake << "\nfullscreen=" << fullscreen
        << "\nhighDpi=" << highDpi << "\nspectatorWindow=" << spectatorWindow
        << "\nverticalSync=" << verticalSync << "\ninvertY=" << invertY
        << "\nsubtitles=" << subtitles << "\nreducedMotion=" << reducedMotion
        << "\nholdToInteract=" << holdToInteract << "\ncolorVisionMode=" << colorVisionMode
        << "\ndisplayMode=" << displayMode << "\nresolutionProfile=" << resolutionProfile
        << "\nsplitScreenLayout=" << splitScreenLayout << "\n";
    return out.str();
}
std::optional<GameSettings> GameSettings::parse(std::string_view text, std::string* error) {
    GameSettings result;
    const auto values = parse_lines(text);
    auto fail = [&](std::string message) -> std::optional<GameSettings> { if (error) *error = std::move(message); return std::nullopt; };
    if (!values.contains("DVE_GAME_SETTINGS")) return fail("missing game-settings version");
    auto number = [&](std::string_view key, auto& target) { const auto it = values.find(std::string(key)); return it == values.end() || parse_number(it->second, target); };
    auto boolean = [&](std::string_view key, bool& target) {
        const auto it = values.find(std::string(key)); if (it == values.end()) return true;
        if (it->second == "1" || it->second == "true") { target = true; return true; }
        if (it->second == "0" || it->second == "false") { target = false; return true; }
        return false;
    };
    if (!number("width", result.width) || !number("height", result.height) || !number("frameRateLimit", result.frameRateLimit) ||
        !number("preferredDisplayId", result.preferredDisplayId) || !number("localPlayerCount", result.localPlayerCount) ||
        !number("renderScale", result.renderScale) || !number("masterVolume", result.masterVolume) ||
        !number("effectsVolume", result.effectsVolume) || !number("musicVolume", result.musicVolume) ||
        !number("fieldOfViewDegrees", result.fieldOfViewDegrees) || !number("mouseSensitivity", result.mouseSensitivity) ||
        !number("uiScale", result.uiScale) || !number("screenShake", result.screenShake) ||
        !boolean("fullscreen", result.fullscreen) || !boolean("highDpi", result.highDpi) ||
        !boolean("spectatorWindow", result.spectatorWindow) || !boolean("verticalSync", result.verticalSync) ||
        !boolean("invertY", result.invertY) || !boolean("subtitles", result.subtitles) ||
        !boolean("reducedMotion", result.reducedMotion) || !boolean("holdToInteract", result.holdToInteract)) return fail("invalid game-settings value");
    if (const auto it = values.find("colorVisionMode"); it != values.end()) result.colorVisionMode = it->second;
    if (const auto it = values.find("displayMode"); it != values.end()) result.displayMode = it->second;
    else result.displayMode = result.fullscreen ? "borderless" : "windowed";
    result.fullscreen = result.displayMode != "windowed";
    if (const auto it = values.find("resolutionProfile"); it != values.end()) result.resolutionProfile = it->second;
    if (const auto it = values.find("splitScreenLayout"); it != values.end()) result.splitScreenLayout = it->second;
    std::string validation;
    if (!result.validate(&validation)) return fail(validation);
    return result;
}

bool InputBindingMap::bind(std::string action, std::string device, std::string control, std::string* error) {
    if (action.empty() || device.empty() || control.empty()) { if (error) *error = "binding fields cannot be empty"; return false; }
    for (const auto& [existingAction, devices] : bindings_) {
        if (existingAction == action) continue;
        const auto it = devices.find(device);
        if (it != devices.end() && it->second == control) { if (error) *error = "control is already bound to " + existingAction; return false; }
    }
    bindings_[std::move(action)][std::move(device)] = std::move(control);
    return true;
}
bool InputBindingMap::unbind(std::string_view action, std::string_view device) {
    const auto actionIt = bindings_.find(std::string(action));
    if (actionIt == bindings_.end()) return false;
    return actionIt->second.erase(std::string(device)) > 0;
}
std::optional<std::string> InputBindingMap::binding(std::string_view action, std::string_view device) const {
    const auto actionIt = bindings_.find(std::string(action));
    if (actionIt == bindings_.end()) return std::nullopt;
    const auto deviceIt = actionIt->second.find(std::string(device));
    return deviceIt == actionIt->second.end() ? std::nullopt : std::optional<std::string>(deviceIt->second);
}

std::vector<std::string> GameMenuModel::actions() const {
    switch (current_) {
    case GameMenuScreen::Closed: return {};
    case GameMenuScreen::Title: return {"Continue", "New Game", "Load", "Missions", "Settings", "Quit"};
    case GameMenuScreen::Pause: return {"Resume", "Save", "Load", "Settings", "Restart Mission", "Return to Title"};
    case GameMenuScreen::Settings: return {"Graphics", "Audio", "Controls", "Accessibility", "Back"};
    case GameMenuScreen::Graphics: return {"Display Mode", "Display", "Resolution Profile", "Resolution", "High DPI", "Split-Screen Layout", "Spectator Window", "Frame Limit", "Render Scale", "Back"};
    case GameMenuScreen::Audio: return {"Master", "Effects", "Music", "Back"};
    case GameMenuScreen::Controls: return {"Keyboard and Mouse", "Gamepad", "Reset Bindings", "Back"};
    case GameMenuScreen::Accessibility: return {"Subtitles", "Reduced Motion", "UI Scale", "Color Vision", "Back"};
    case GameMenuScreen::SaveLoad: return {"Save Slot 1", "Save Slot 2", "Autosave", "Back"};
    case GameMenuScreen::MissionSelect: return {"Mission List", "Back"};
    case GameMenuScreen::QuitConfirmation: return {"Cancel", "Quit"};
    }
    return {};
}

void GameHudModel::set_tools(std::vector<ToolWheelEntry> tools) { tools_ = std::move(tools); selected_ = 0; }
bool GameHudModel::select_next_tool() {
    if (tools_.empty()) return false;
    for (std::size_t offset = 1; offset <= tools_.size(); ++offset) {
        const std::size_t candidate = (selected_ + offset) % tools_.size();
        if (tools_[candidate].enabled) { selected_ = candidate; return true; }
    }
    return false;
}
std::optional<ToolWheelEntry> GameHudModel::selected_tool() const {
    if (tools_.empty() || selected_ >= tools_.size() || !tools_[selected_].enabled) return std::nullopt;
    return tools_[selected_];
}

void UiDataModel::set(std::string key, UiValue value) {
    if (key.empty() || key.size() > 255U) return;
    if (const double* number = std::get_if<double>(&value); number && !std::isfinite(*number)) return;
    values_.insert_or_assign(std::move(key), std::move(value));
}

const UiValue* UiDataModel::get(std::string_view key) const noexcept {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
}

UiDocument::UiDocument() {
    UiWidget root;
    root.id = root_;
    root.kind = UiWidgetKind::CanvasRoot;
    root.name = "Root";
    root.layout.direction = UiLayoutDirection::Overlay;
    widgets_.emplace(root_, std::move(root));
}

UiWidgetId UiDocument::add_widget(
    UiWidgetId parent, UiWidgetKind kind, std::string name, std::string* error) {
    const auto parentWidget = widgets_.find(parent);
    if (parentWidget == widgets_.end()) { if (error) *error = "UI parent widget does not exist"; return kInvalidUiWidgetId; }
    if (name.empty() || name.size() > 255U) { if (error) *error = "UI widget name is invalid"; return kInvalidUiWidgetId; }
    for (const auto& [id, widget] : widgets_) {
        (void)id;
        if (widget.name == name) { if (error) *error = "UI widget name is not unique"; return kInvalidUiWidgetId; }
    }
    UiWidget widget;
    widget.id = nextId_++;
    widget.parent = parent;
    widget.kind = kind;
    widget.name = std::move(name);
    widget.focusable = kind == UiWidgetKind::Button || kind == UiWidgetKind::Slider ||
        kind == UiWidgetKind::TextInput || kind == UiWidgetKind::List;
    const UiWidgetId id = widget.id;
    widgets_.emplace(id, std::move(widget));
    parentWidget->second.children.push_back(id);
    return id;
}

bool UiDocument::remove_widget(UiWidgetId id) {
    if (id == root_ || !widgets_.contains(id)) return false;
    std::vector<UiWidgetId> pending{id};
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto found = widgets_.find(pending[cursor]);
        if (found != widgets_.end())
            for (UiWidgetId child : found->second.children) pending.push_back(child);
    }
    const UiWidgetId parent = widgets_.at(id).parent;
    if (auto parentWidget = widgets_.find(parent); parentWidget != widgets_.end())
        std::erase(parentWidget->second.children, id);
    for (UiWidgetId removed : pending) widgets_.erase(removed);
    return true;
}

UiWidget* UiDocument::widget(UiWidgetId id) noexcept {
    const auto found = widgets_.find(id);
    return found == widgets_.end() ? nullptr : &found->second;
}

const UiWidget* UiDocument::widget(UiWidgetId id) const noexcept {
    const auto found = widgets_.find(id);
    return found == widgets_.end() ? nullptr : &found->second;
}

UiWidgetId UiDocument::find_widget(std::string_view name) const noexcept {
    const auto found = std::find_if(widgets_.begin(), widgets_.end(), [&](const auto& item) {
        return item.second.name == name;
    });
    return found == widgets_.end() ? kInvalidUiWidgetId : found->first;
}

bool UiDocument::replace_records(
    UiWidgetId root, UiWidgetId nextId, std::map<UiWidgetId, UiWidget> widgets,
    std::string* error) {
    UiDocument candidate;
    candidate.root_ = root;
    candidate.nextId_ = nextId;
    candidate.widgets_ = std::move(widgets);
    if (candidate.nextId_ <= candidate.root_ || !candidate.validate(error)) return false;
    for (const auto& [id, widget] : candidate.widgets_) {
        (void)widget;
        if (id >= candidate.nextId_) {
            if (error) *error = "UI document next widget ID is not greater than every record";
            return false;
        }
    }
    *this = std::move(candidate);
    return true;
}

bool UiDocument::is_descendant(UiWidgetId widgetId, UiWidgetId ancestor) const noexcept {
    if (ancestor == kInvalidUiWidgetId) return true;
    auto found = widgets_.find(widgetId);
    while (found != widgets_.end()) {
        if (found->first == ancestor) return true;
        if (found->second.parent == kInvalidUiWidgetId) break;
        found = widgets_.find(found->second.parent);
    }
    return false;
}

bool UiDocument::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    const auto rootWidget = widgets_.find(root_);
    if (rootWidget == widgets_.end() || rootWidget->second.parent != kInvalidUiWidgetId ||
        rootWidget->second.kind != UiWidgetKind::CanvasRoot) return fail("UI document root is invalid");
    std::set<std::string, std::less<>> names;
    for (const auto& [id, widget] : widgets_) {
        if (id != widget.id || widget.name.empty() || widget.name.size() > 255U ||
            widget.text.size() > 65535U || widget.localizationKey.size() > 255U ||
            widget.accessibilityLabel.size() > 4096U || widget.imageAsset.size() > 4096U ||
            widget.binding.size() > 255U ||
            static_cast<unsigned>(widget.kind) > static_cast<unsigned>(UiWidgetKind::TextInput) ||
            static_cast<unsigned>(widget.bindingTarget) > static_cast<unsigned>(UiBindingTarget::Enabled) ||
            static_cast<unsigned>(widget.layout.direction) > static_cast<unsigned>(UiLayoutDirection::Overlay) ||
            !names.insert(widget.name).second)
            return fail("UI widget identity or name is invalid");
        if (id != root_ && !widgets_.contains(widget.parent)) return fail("UI widget parent is missing");
        if (!std::isfinite(widget.layout.position.x) || !std::isfinite(widget.layout.position.y) ||
            !std::isfinite(widget.layout.preferredSize.x) || !std::isfinite(widget.layout.preferredSize.y) ||
            widget.layout.preferredSize.x < 0.0F || widget.layout.preferredSize.y < 0.0F ||
            !std::isfinite(widget.layout.minimumSize.x) || !std::isfinite(widget.layout.minimumSize.y) ||
            !std::isfinite(widget.layout.maximumSize.x) || !std::isfinite(widget.layout.maximumSize.y) ||
            widget.layout.minimumSize.x < 0.0F || widget.layout.minimumSize.y < 0.0F ||
            widget.layout.maximumSize.x < widget.layout.minimumSize.x ||
            widget.layout.maximumSize.y < widget.layout.minimumSize.y ||
            !std::isfinite(widget.layout.spacing) || widget.layout.spacing < 0.0F ||
            !std::isfinite(widget.layout.flexGrow) || widget.layout.flexGrow < 0.0F ||
            !std::isfinite(widget.value) || !std::isfinite(widget.minimum) || !std::isfinite(widget.maximum) ||
            widget.minimum > widget.maximum || !std::isfinite(widget.step) || widget.step < 0.0 ||
            !std::isfinite(widget.color.r) || !std::isfinite(widget.color.g) ||
            !std::isfinite(widget.color.b) || !std::isfinite(widget.color.a))
            return fail("UI widget layout or value range is invalid");
        const std::array<float, 8> edges{
            widget.layout.margin.left, widget.layout.margin.top, widget.layout.margin.right,
            widget.layout.margin.bottom, widget.layout.padding.left, widget.layout.padding.top,
            widget.layout.padding.right, widget.layout.padding.bottom};
        if (std::any_of(edges.begin(), edges.end(), [](float edge) {
                return !std::isfinite(edge) || edge < 0.0F;
            })) return fail("UI widget margin or padding is invalid");
        std::set<UiWidgetId> childIds;
        for (UiWidgetId child : widget.children) {
            const auto childWidget = widgets_.find(child);
            if (childWidget == widgets_.end() || childWidget->second.parent != id || !childIds.insert(child).second)
                return fail("UI child relationship is invalid");
        }
    }
    std::set<UiWidgetId> visited;
    std::function<bool(UiWidgetId)> visit = [&](UiWidgetId id) {
        if (!visited.insert(id).second) return false;
        for (UiWidgetId child : widgets_.at(id).children) if (!visit(child)) return false;
        return true;
    };
    if (!visit(root_) || visited.size() != widgets_.size()) return fail("UI document contains a cycle or orphan");
    return true;
}

std::vector<UiDrawCommand> UiDocument::build_draw_list(
    UiCanvasId canvasId, const UiCanvasDesc& canvas, const UiDataModel& data,
    UiWidgetId focused, std::string* error) const {
    if (!validate(error) || !canvas.visible || !std::isfinite(canvas.logicalSize.x) ||
        !std::isfinite(canvas.logicalSize.y) || canvas.logicalSize.x <= 0.0F ||
        canvas.logicalSize.y <= 0.0F || !std::isfinite(canvas.metersPerLogicalUnit) ||
        canvas.metersPerLogicalUnit <= 0.0F)
        return {};
    auto intersection = [](UiRect a, UiRect b) {
        const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        const float x1 = std::min(a.x + a.width, b.x + b.width);
        const float y1 = std::min(a.y + a.height, b.y + b.height);
        return UiRect{x0, y0, std::max(0.0F, x1 - x0), std::max(0.0F, y1 - y0)};
    };
    auto clamp_size = [](float value, float minimum, float maximum) {
        return std::clamp(value, std::max(0.0F, minimum), std::max(minimum, maximum));
    };
    std::vector<UiDrawCommand> result;
    std::function<void(UiWidgetId, UiRect, UiRect)> layoutWidget;
    layoutWidget = [&](UiWidgetId id, UiRect rectangle, UiRect clip) {
        const UiWidget& source = widgets_.at(id);
        bool visible = source.visible;
        bool enabled = source.enabled;
        std::string text = source.text;
        double value = source.value;
        if (!source.binding.empty()) {
            if (const UiValue* bound = data.get(source.binding)) {
                if (source.bindingTarget == UiBindingTarget::Text)
                    text = std::visit([](const auto& item) { std::ostringstream out; out << item; return out.str(); }, *bound);
                else if (source.bindingTarget == UiBindingTarget::Value) {
                    if (const double* number = std::get_if<double>(bound)) value = *number;
                    else if (const std::int64_t* integer = std::get_if<std::int64_t>(bound)) value = static_cast<double>(*integer);
                } else if (source.bindingTarget == UiBindingTarget::Visible) {
                    if (const bool* flag = std::get_if<bool>(bound)) visible = *flag;
                } else if (source.bindingTarget == UiBindingTarget::Enabled) {
                    if (const bool* flag = std::get_if<bool>(bound)) enabled = *flag;
                }
            }
        }
        if (!visible) return;
        const UiRect clipped = intersection(rectangle, clip);
        if (id != root_ && clipped.width > 0.0F && clipped.height > 0.0F) {
            UiDrawCommand command;
            command.canvas = canvasId;
            command.widget = id;
            command.kind = source.kind;
            command.rectangle = rectangle;
            command.clipRectangle = clipped;
            command.color = source.color;
            command.text = std::move(text);
            command.imageAsset = source.imageAsset;
            command.localizationKey = source.localizationKey;
            command.value = std::clamp(value, source.minimum, source.maximum);
            command.canvasMode = canvas.mode;
            command.worldTransform = canvas.worldTransform;
            command.layer = canvas.layer;
            command.enabled = enabled;
            command.focused = enabled && id == focused;
            result.push_back(std::move(command));
        }
        const UiRect content{
            rectangle.x + source.layout.padding.left,
            rectangle.y + source.layout.padding.top,
            std::max(0.0F, rectangle.width - source.layout.padding.left - source.layout.padding.right),
            std::max(0.0F, rectangle.height - source.layout.padding.top - source.layout.padding.bottom)};
        const UiLayoutDirection direction = source.kind == UiWidgetKind::List
            ? UiLayoutDirection::Vertical : source.layout.direction;
        float totalFixed{}, totalGrow{};
        if (direction == UiLayoutDirection::Horizontal || direction == UiLayoutDirection::Vertical) {
            for (UiWidgetId childId : source.children) {
                const UiWidget& child = widgets_.at(childId);
                const bool horizontal = direction == UiLayoutDirection::Horizontal;
                totalFixed += horizontal ? child.layout.preferredSize.x + child.layout.margin.left + child.layout.margin.right
                                         : child.layout.preferredSize.y + child.layout.margin.top + child.layout.margin.bottom;
                totalGrow += child.layout.flexGrow;
            }
            if (!source.children.empty()) totalFixed += source.layout.spacing * static_cast<float>(source.children.size() - 1U);
        }
        float cursor = direction == UiLayoutDirection::Horizontal ? content.x : content.y;
        const float availableMain = direction == UiLayoutDirection::Horizontal ? content.width : content.height;
        const float extra = std::max(0.0F, availableMain - totalFixed);
        for (UiWidgetId childId : source.children) {
            const UiWidget& child = widgets_.at(childId);
            UiRect childRect{};
            if (direction == UiLayoutDirection::Absolute) {
                childRect = {content.x + child.layout.position.x + child.layout.margin.left,
                             content.y + child.layout.position.y + child.layout.margin.top,
                             clamp_size(child.layout.preferredSize.x, child.layout.minimumSize.x, child.layout.maximumSize.x),
                             clamp_size(child.layout.preferredSize.y, child.layout.minimumSize.y, child.layout.maximumSize.y)};
            } else if (direction == UiLayoutDirection::Overlay) {
                childRect = {content.x + child.layout.margin.left, content.y + child.layout.margin.top,
                             std::max(0.0F, content.width - child.layout.margin.left - child.layout.margin.right),
                             std::max(0.0F, content.height - child.layout.margin.top - child.layout.margin.bottom)};
            } else if (direction == UiLayoutDirection::Horizontal) {
                const float width = clamp_size(child.layout.preferredSize.x +
                    (totalGrow > 0.0F ? extra * child.layout.flexGrow / totalGrow : 0.0F),
                    child.layout.minimumSize.x, child.layout.maximumSize.x);
                childRect = {cursor + child.layout.margin.left, content.y + child.layout.margin.top, width,
                             clamp_size(content.height - child.layout.margin.top - child.layout.margin.bottom,
                                        child.layout.minimumSize.y, child.layout.maximumSize.y)};
                cursor += width + child.layout.margin.left + child.layout.margin.right + source.layout.spacing;
            } else {
                const float height = clamp_size(child.layout.preferredSize.y +
                    (totalGrow > 0.0F ? extra * child.layout.flexGrow / totalGrow : 0.0F),
                    child.layout.minimumSize.y, child.layout.maximumSize.y);
                childRect = {content.x + child.layout.margin.left, cursor + child.layout.margin.top,
                             clamp_size(content.width - child.layout.margin.left - child.layout.margin.right,
                                        child.layout.minimumSize.x, child.layout.maximumSize.x), height};
                cursor += height + child.layout.margin.top + child.layout.margin.bottom + source.layout.spacing;
            }
            layoutWidget(childId, childRect, clipped);
        }
    };
    const UiRect viewport{0.0F, 0.0F, canvas.logicalSize.x, canvas.logicalSize.y};
    layoutWidget(root_, viewport, viewport);
    return result;
}

UiWidgetId UiDocument::first_focusable(const UiDataModel* data) const noexcept {
    return next_focusable(kInvalidUiWidgetId, false, data);
}

UiWidgetId UiDocument::next_focusable(
    UiWidgetId current, bool reverse, const UiDataModel* data, UiWidgetId scopeRoot) const noexcept {
    std::vector<UiWidgetId> order;
    std::function<void(UiWidgetId)> collect = [&](UiWidgetId id) {
        const auto found = widgets_.find(id);
        if (found == widgets_.end()) return;
        bool visible = found->second.visible;
        bool enabled = found->second.enabled;
        if (data && !found->second.binding.empty()) {
            if (const UiValue* bound = data->get(found->second.binding)) {
                if (found->second.bindingTarget == UiBindingTarget::Visible)
                    if (const bool* value = std::get_if<bool>(bound)) visible = *value;
                if (found->second.bindingTarget == UiBindingTarget::Enabled)
                    if (const bool* value = std::get_if<bool>(bound)) enabled = *value;
            }
        }
        if (found->second.focusable && visible && enabled) order.push_back(id);
        if (!visible) return;
        for (UiWidgetId child : found->second.children) collect(child);
    };
    collect(scopeRoot == kInvalidUiWidgetId ? root_ : scopeRoot);
    if (order.empty()) return kInvalidUiWidgetId;
    const auto found = std::find(order.begin(), order.end(), current);
    if (found == order.end()) return reverse ? order.back() : order.front();
    const std::size_t index = static_cast<std::size_t>(found - order.begin());
    return reverse ? order[(index + order.size() - 1U) % order.size()] : order[(index + 1U) % order.size()];
}

UiCanvasId UiRuntime::create_canvas(UiCanvasDesc desc, std::string* error) {
    const Quaternion rotation = desc.worldTransform.rotation;
    const float rotationLengthSquared = rotation.x * rotation.x + rotation.y * rotation.y +
        rotation.z * rotation.z + rotation.w * rotation.w;
    if (desc.name.empty() || !std::isfinite(desc.logicalSize.x) || !std::isfinite(desc.logicalSize.y) ||
        desc.logicalSize.x <= 0.0F || desc.logicalSize.y <= 0.0F ||
        !std::isfinite(desc.metersPerLogicalUnit) || desc.metersPerLogicalUnit <= 0.0F ||
        !std::isfinite(desc.worldTransform.position.x) || !std::isfinite(desc.worldTransform.position.y) ||
        !std::isfinite(desc.worldTransform.position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
        !(rotationLengthSquared > 0.0F)) {
        if (error) *error = "UI canvas description is invalid";
        return kInvalidUiCanvasId;
    }
    const UiCanvasId id = nextCanvasId_++;
    CanvasRecord record;
    record.desc = std::move(desc);
    canvases_.emplace(id, std::move(record));
    return id;
}

UiCanvasId UiRuntime::load_asset(const std::filesystem::path& path, std::string* error) {
    UiAssetReadResult loaded = read_dveui(path);
    if (!loaded) {
        if (error) *error = loaded.error;
        return kInvalidUiCanvasId;
    }
    CanvasRecord record;
    record.desc = std::move(loaded.asset.canvas);
    record.document = std::move(loaded.asset.document);
    record.sourcePath = path.lexically_normal();
    record.sourceHash = loaded.asset.contentHash;
    std::error_code ec;
    record.sourceWriteTime = std::filesystem::last_write_time(record.sourcePath, ec);
    if (ec) {
        if (error) *error = "could not query UI asset write time";
        return kInvalidUiCanvasId;
    }
    const UiCanvasId id = nextCanvasId_++;
    canvases_.emplace(id, std::move(record));
    rebuild();
    return id;
}

bool UiRuntime::reload_asset(UiCanvasId id, std::string* error) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end() || found->second.sourcePath.empty()) {
        if (error) *error = "UI canvas is not backed by a serialized asset";
        return false;
    }
    UiAssetReadResult loaded = read_dveui(found->second.sourcePath);
    if (!loaded) {
        if (error) *error = loaded.error;
        return false;
    }
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(found->second.sourcePath, ec);
    if (ec) {
        if (error) *error = "could not query UI asset write time";
        return false;
    }
    CanvasRecord& record = found->second;
    const UiWidget* oldFocus = record.document.widget(record.focused);
    const UiWidget* oldModal = record.document.widget(record.modalRoot);
    const std::string focusName = oldFocus ? oldFocus->name : std::string{};
    const std::string modalName = oldModal ? oldModal->name : std::string{};
    record.desc = std::move(loaded.asset.canvas);
    record.document = std::move(loaded.asset.document);
    record.sourceHash = loaded.asset.contentHash;
    record.sourceWriteTime = writeTime;
    record.focused = focusName.empty() ? kInvalidUiWidgetId : record.document.find_widget(focusName);
    record.modalRoot = modalName.empty() ? kInvalidUiWidgetId : record.document.find_widget(modalName);
    if (record.modalRoot != kInvalidUiWidgetId) {
        const UiWidget* modal = record.document.widget(record.modalRoot);
        if (!modal || !modal->modal) record.modalRoot = kInvalidUiWidgetId;
    }
    if (record.focused != kInvalidUiWidgetId) {
        const UiWidget* focus = record.document.widget(record.focused);
        if (!focus || !focus->focusable ||
            !record.document.is_descendant(record.focused, record.modalRoot))
            record.focused = kInvalidUiWidgetId;
    }
    record.hovered = kInvalidUiWidgetId;
    record.captured = kInvalidUiWidgetId;
    rebuild();
    return true;
}

std::size_t UiRuntime::reload_changed_assets() {
    std::vector<UiCanvasId> changed;
    for (const auto& [id, record] : canvases_) {
        if (record.sourcePath.empty()) continue;
        std::error_code ec;
        const auto writeTime = std::filesystem::last_write_time(record.sourcePath, ec);
        if (!ec && writeTime != record.sourceWriteTime) changed.push_back(id);
    }
    std::size_t reloaded{};
    for (UiCanvasId id : changed) if (reload_asset(id, nullptr)) ++reloaded;
    return reloaded;
}

bool UiRuntime::destroy_canvas(UiCanvasId id) { return canvases_.erase(id) != 0U; }
UiDocument* UiRuntime::document(UiCanvasId id) noexcept { const auto it = canvases_.find(id); return it == canvases_.end() ? nullptr : &it->second.document; }
const UiDocument* UiRuntime::document(UiCanvasId id) const noexcept { const auto it = canvases_.find(id); return it == canvases_.end() ? nullptr : &it->second.document; }
UiCanvasDesc* UiRuntime::canvas(UiCanvasId id) noexcept { const auto it = canvases_.find(id); return it == canvases_.end() ? nullptr : &it->second.desc; }
const UiCanvasDesc* UiRuntime::canvas(UiCanvasId id) const noexcept { const auto it = canvases_.find(id); return it == canvases_.end() ? nullptr : &it->second.desc; }

bool UiRuntime::navigate(UiCanvasId id, UiNavigation navigation) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end()) return false;
    rebuild();
    CanvasRecord& record = found->second;
    const UiWidgetId previousFocus = record.focused;
    const bool acquiredFocus = record.focused == kInvalidUiWidgetId;
    if (acquiredFocus)
        record.focused = record.document.next_focusable(
            kInvalidUiWidgetId, false, &data_, record.modalRoot);
    const bool focusOnly = acquiredFocus && (
        navigation == UiNavigation::Next || navigation == UiNavigation::Previous ||
        navigation == UiNavigation::Up || navigation == UiNavigation::Down ||
        navigation == UiNavigation::Left || navigation == UiNavigation::Right);
    if (focusOnly) {
        if (UiWidget* widget = record.document.widget(record.focused);
            widget && widget->kind == UiWidgetKind::TextInput)
            record.textCaretBytes.try_emplace(widget->id, widget->text.size());
        if (record.focused != previousFocus) emit_focus_event(id, record.focused);
        rebuild();
        return record.focused != kInvalidUiWidgetId;
    }

    if (navigation == UiNavigation::Next)
        record.focused = record.document.next_focusable(record.focused, false, &data_, record.modalRoot);
    else if (navigation == UiNavigation::Previous)
        record.focused = record.document.next_focusable(record.focused, true, &data_, record.modalRoot);
    else if (UiWidget* widget = record.document.widget(record.focused)) {
        auto caret = [&]() -> std::size_t& {
            auto [it, inserted] = record.textCaretBytes.try_emplace(widget->id, widget->text.size());
            if (!inserted) it->second = std::min(it->second, widget->text.size());
            return it->second;
        };
        if ((navigation == UiNavigation::Left || navigation == UiNavigation::Right) &&
            widget->kind == UiWidgetKind::TextInput) {
            std::size_t& position = caret();
            position = navigation == UiNavigation::Left
                ? previous_utf8_boundary(widget->text, position)
                : next_utf8_boundary(widget->text, position);
        } else if ((navigation == UiNavigation::Left || navigation == UiNavigation::Right) &&
                   widget->kind == UiWidgetKind::Slider) {
            const double sign = navigation == UiNavigation::Right ? 1.0 : -1.0;
            const double range = std::max(0.0, widget->maximum - widget->minimum);
            const double step = widget->step > 0.0 ? widget->step : range / 20.0;
            if (!(step > 0.0)) return false;
            widget->value = std::clamp(widget->value + sign * step,
                                       widget->minimum, widget->maximum);
            events_.push_back({id, widget->id, "change", widget->value});
        } else if (navigation == UiNavigation::Left || navigation == UiNavigation::Right ||
                   navigation == UiNavigation::Up || navigation == UiNavigation::Down) {
            const auto current = std::find_if(drawCommands_.begin(), drawCommands_.end(),
                [&](const UiDrawCommand& command) {
                    return command.canvas == id && command.widget == record.focused;
                });
            if (current == drawCommands_.end()) return false;
            const float currentX = current->rectangle.x + current->rectangle.width * 0.5F;
            const float currentY = current->rectangle.y + current->rectangle.height * 0.5F;
            UiWidgetId best = kInvalidUiWidgetId;
            float bestScore = std::numeric_limits<float>::infinity();
            for (const UiDrawCommand& candidate : drawCommands_) {
                if (candidate.canvas != id || candidate.widget == record.focused ||
                    !candidate.enabled ||
                    !record.document.is_descendant(candidate.widget, record.modalRoot)) continue;
                const UiWidget* candidateWidget = record.document.widget(candidate.widget);
                if (!candidateWidget || !interactive_widget(*candidateWidget)) continue;
                const float dx = candidate.rectangle.x + candidate.rectangle.width * 0.5F - currentX;
                const float dy = candidate.rectangle.y + candidate.rectangle.height * 0.5F - currentY;
                float primary{}, lateral{};
                if (navigation == UiNavigation::Right) { primary = dx; lateral = dy; }
                else if (navigation == UiNavigation::Left) { primary = -dx; lateral = dy; }
                else if (navigation == UiNavigation::Down) { primary = dy; lateral = dx; }
                else { primary = -dy; lateral = dx; }
                if (primary <= 1.0e-4F) continue;
                const float distance = std::sqrt(primary * primary + lateral * lateral);
                const float alignmentPenalty = 1.0F - primary / std::max(distance, 1.0e-5F);
                const float score = distance * (1.0F + 3.0F * alignmentPenalty);
                if (score < bestScore || (score == bestScore && candidate.widget < best)) {
                    bestScore = score;
                    best = candidate.widget;
                }
            }
            if (best == kInvalidUiWidgetId) return false;
            record.focused = best;
        } else if (navigation == UiNavigation::Activate) {
            const auto command = std::find_if(drawCommands_.begin(), drawCommands_.end(),
                [&](const UiDrawCommand& item) {
                    return item.canvas == id && item.widget == widget->id;
                });
            if (command == drawCommands_.end() || !command->enabled) return false;
            events_.push_back({id, widget->id, "activate", widget->value});
        } else return false;
    }
    if (record.focused != previousFocus) {
        if (UiWidget* widget = record.document.widget(record.focused);
            widget && widget->kind == UiWidgetKind::TextInput)
            record.textCaretBytes.try_emplace(widget->id, widget->text.size());
        emit_focus_event(id, record.focused);
    }
    rebuild();
    return record.focused != kInvalidUiWidgetId;
}

UiWidgetId UiRuntime::hit_test(
    const CanvasRecord& record, UiCanvasId id, float x, float y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) return kInvalidUiWidgetId;
    const auto contains = [x, y](const UiRect& rectangle) {
        return x >= rectangle.x && y >= rectangle.y &&
            x < rectangle.x + rectangle.width && y < rectangle.y + rectangle.height;
    };
    for (auto command = drawCommands_.rbegin(); command != drawCommands_.rend(); ++command) {
        if (command->canvas != id || !command->enabled || !contains(command->rectangle) ||
            !contains(command->clipRectangle) ||
            !record.document.is_descendant(command->widget, record.modalRoot)) continue;
        const UiWidget* widget = record.document.widget(command->widget);
        if (widget && interactive_widget(*widget)) return widget->id;
    }
    return kInvalidUiWidgetId;
}

void UiRuntime::emit_focus_event(UiCanvasId id, UiWidgetId widgetId) {
    if (widgetId == kInvalidUiWidgetId) return;
    const auto canvasFound = canvases_.find(id);
    if (canvasFound == canvases_.end()) return;
    const UiWidget* widget = canvasFound->second.document.widget(widgetId);
    if (!widget) return;
    const std::string label = !widget->accessibilityLabel.empty()
        ? widget->accessibilityLabel : (!widget->text.empty() ? widget->text : widget->name);
    accessibilityEvents_.push_back({id, widgetId, "focus", label});
}

bool UiRuntime::pointer_move(UiCanvasId id, float x, float y) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end()) return false;
    rebuild();
    CanvasRecord& record = found->second;
    record.hovered = hit_test(record, id, x, y);
    if (record.captured == kInvalidUiWidgetId) return record.hovered != kInvalidUiWidgetId;
    UiWidget* widget = record.document.widget(record.captured);
    if (!widget || widget->kind != UiWidgetKind::Slider) return true;
    const auto command = std::find_if(drawCommands_.begin(), drawCommands_.end(), [&](const UiDrawCommand& item) {
        return item.canvas == id && item.widget == widget->id;
    });
    if (command == drawCommands_.end() || command->rectangle.width <= 0.0F) return true;
    const double normalized = std::clamp(
        static_cast<double>((x - command->rectangle.x) / command->rectangle.width), 0.0, 1.0);
    double value = widget->minimum + normalized * (widget->maximum - widget->minimum);
    if (widget->step > 0.0)
        value = widget->minimum + std::round((value - widget->minimum) / widget->step) * widget->step;
    value = std::clamp(value, widget->minimum, widget->maximum);
    if (value != widget->value) {
        widget->value = value;
        events_.push_back({id, widget->id, "change", value});
        rebuild();
    }
    return true;
}

bool UiRuntime::pointer_down(UiCanvasId id, float x, float y) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end()) return false;
    rebuild();
    CanvasRecord& record = found->second;
    const UiWidgetId hit = hit_test(record, id, x, y);
    record.hovered = hit;
    record.captured = hit;
    if (hit == kInvalidUiWidgetId) return false;
    if (record.focused != hit) {
        record.focused = hit;
        emit_focus_event(id, hit);
    }
    UiWidget* widget = record.document.widget(hit);
    if (widget && widget->kind == UiWidgetKind::Slider) return pointer_move(id, x, y);
    if (widget && widget->kind == UiWidgetKind::TextInput) {
        const auto command = std::find_if(drawCommands_.begin(), drawCommands_.end(),
            [&](const UiDrawCommand& item) { return item.canvas == id && item.widget == hit; });
        const std::size_t count = utf8_codepoint_count(widget->text, widget->text.size());
        if (command != drawCommands_.end() && command->rectangle.width > 0.0F) {
            const float normalized = std::clamp(
                (x - command->rectangle.x) / command->rectangle.width, 0.0F, 1.0F);
            const auto index = static_cast<std::size_t>(std::lround(
                normalized * static_cast<float>(count)));
            record.textCaretBytes[widget->id] = utf8_boundary_for_codepoint(widget->text, index);
        } else record.textCaretBytes[widget->id] = widget->text.size();
    }
    rebuild();
    return true;
}

bool UiRuntime::pointer_up(UiCanvasId id, float x, float y) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end()) return false;
    rebuild();
    CanvasRecord& record = found->second;
    const UiWidgetId captured = record.captured;
    const UiWidgetId hit = hit_test(record, id, x, y);
    record.hovered = hit;
    record.captured = kInvalidUiWidgetId;
    if (captured == kInvalidUiWidgetId) return false;
    UiWidget* widget = record.document.widget(captured);
    if (widget && captured == hit &&
        (widget->kind == UiWidgetKind::Button || widget->kind == UiWidgetKind::List))
        events_.push_back({id, captured, "activate", widget->value});
    else if (widget && widget->kind == UiWidgetKind::Slider)
        events_.push_back({id, captured, "commit", widget->value});
    rebuild();
    return true;
}

bool UiRuntime::key_down(UiCanvasId id, std::string_view key, bool shift) {
    std::string normalized(key);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const auto found = canvases_.find(id);
    if (found == canvases_.end()) return false;
    CanvasRecord& record = found->second;
    if (normalized == "escape") {
        if (record.modalRoot != kInvalidUiWidgetId) {
            events_.push_back({id, record.modalRoot, "cancel", false});
            record.modalRoot = kInvalidUiWidgetId;
            record.captured = kInvalidUiWidgetId;
            rebuild();
            return true;
        }
        return false;
    }
    UiWidget* widget = record.document.widget(record.focused);
    const auto focusedCommand = std::find_if(drawCommands_.begin(), drawCommands_.end(),
        [&](const UiDrawCommand& command) {
            return command.canvas == id && command.widget == record.focused;
        });
    const bool focusedEnabled = focusedCommand != drawCommands_.end() && focusedCommand->enabled;
    if (widget && widget->kind == UiWidgetKind::TextInput) {
        if (!focusedEnabled) return false;
        auto [caretIt, inserted] = record.textCaretBytes.try_emplace(widget->id, widget->text.size());
        if (!inserted) caretIt->second = std::min(caretIt->second, widget->text.size());
        std::size_t& caret = caretIt->second;
        if (normalized == "left") caret = previous_utf8_boundary(widget->text, caret);
        else if (normalized == "right") caret = next_utf8_boundary(widget->text, caret);
        else if (normalized == "home") caret = 0U;
        else if (normalized == "end") caret = widget->text.size();
        else if (normalized == "backspace") {
            if (caret == 0U) return false;
            const std::size_t eraseAt = previous_utf8_boundary(widget->text, caret);
            widget->text.erase(eraseAt, caret - eraseAt);
            caret = eraseAt;
            events_.push_back({id, widget->id, "text", widget->text});
        } else if (normalized == "delete") {
            if (caret >= widget->text.size()) return false;
            const std::size_t eraseEnd = next_utf8_boundary(widget->text, caret);
            widget->text.erase(caret, eraseEnd - caret);
            events_.push_back({id, widget->id, "text", widget->text});
        } else if (normalized == "enter" || normalized == "return") {
            events_.push_back({id, widget->id, "commit", widget->text});
        } else if (normalized == "space") {
            // Text characters arrive through text_input; returning false prevents activation and
            // avoids inserting the same space twice on platforms that send both event types.
            return false;
        } else if (normalized != "left" && normalized != "right" &&
                   normalized != "home" && normalized != "end") return false;
        rebuild();
        return true;
    }
    if (normalized == "tab") return navigate(id, shift ? UiNavigation::Previous : UiNavigation::Next);
    if (normalized == "left") return navigate(id, UiNavigation::Left);
    if (normalized == "right") return navigate(id, UiNavigation::Right);
    if (normalized == "up") return navigate(id, UiNavigation::Up);
    if (normalized == "down") return navigate(id, UiNavigation::Down);
    if (normalized == "enter" || normalized == "return" || normalized == "space")
        return navigate(id, UiNavigation::Activate);
    return false;
}

bool UiRuntime::action(UiCanvasId id, std::string_view actionName) {
    if (actionName == "ui.next") return navigate(id, UiNavigation::Next);
    if (actionName == "ui.previous") return navigate(id, UiNavigation::Previous);
    if (actionName == "ui.left") return navigate(id, UiNavigation::Left);
    if (actionName == "ui.right") return navigate(id, UiNavigation::Right);
    if (actionName == "ui.up") return navigate(id, UiNavigation::Up);
    if (actionName == "ui.down") return navigate(id, UiNavigation::Down);
    if (actionName == "ui.accept") return navigate(id, UiNavigation::Activate);
    if (actionName == "ui.cancel") return key_down(id, "escape");
    return false;
}

bool UiRuntime::text_input(UiCanvasId id, std::string_view text) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end() || text.empty()) return false;
    CanvasRecord& record = found->second;
    UiWidget* widget = record.document.widget(record.focused);
    const auto command = std::find_if(drawCommands_.begin(), drawCommands_.end(),
        [&](const UiDrawCommand& item) {
            return item.canvas == id && item.widget == record.focused;
        });
    if (!widget || widget->kind != UiWidgetKind::TextInput ||
        command == drawCommands_.end() || !command->enabled) return false;
    std::string filtered = filter_single_line_text(text);
    if (filtered.empty() || widget->text.size() + filtered.size() > 4096U) return false;
    auto [caretIt, inserted] = record.textCaretBytes.try_emplace(widget->id, widget->text.size());
    if (!inserted) caretIt->second = std::min(caretIt->second, widget->text.size());
    widget->text.insert(caretIt->second, filtered);
    caretIt->second += filtered.size();
    events_.push_back({id, widget->id, "text", widget->text});
    rebuild();
    return true;
}

bool UiRuntime::set_modal_root(UiCanvasId id, UiWidgetId root) {
    const auto found = canvases_.find(id);
    if (found == canvases_.end()) return false;
    CanvasRecord& record = found->second;
    if (root != kInvalidUiWidgetId) {
        const UiWidget* widget = record.document.widget(root);
        if (!widget || !widget->modal) return false;
    }
    record.modalRoot = root;
    record.captured = kInvalidUiWidgetId;
    record.hovered = kInvalidUiWidgetId;
    record.focused = record.document.next_focusable(kInvalidUiWidgetId, false, &data_, root);
    emit_focus_event(id, record.focused);
    rebuild();
    return true;
}

UiWidgetId UiRuntime::hovered_widget(UiCanvasId id) const noexcept {
    const auto found = canvases_.find(id);
    return found == canvases_.end() ? kInvalidUiWidgetId : found->second.hovered;
}

UiWidgetId UiRuntime::captured_widget(UiCanvasId id) const noexcept {
    const auto found = canvases_.find(id);
    return found == canvases_.end() ? kInvalidUiWidgetId : found->second.captured;
}

std::vector<UiEvent> UiRuntime::take_events() {
    std::vector<UiEvent> result;
    result.swap(events_);
    return result;
}

std::vector<UiEvent> UiRuntime::take_accessibility_events() {
    std::vector<UiEvent> result;
    result.swap(accessibilityEvents_);
    return result;
}

void UiRuntime::rebuild() {
    drawCommands_.clear();
    for (const auto& [id, record] : canvases_) {
        std::string ignored;
        auto commands = record.document.build_draw_list(id, record.desc, data_, record.focused, &ignored);
        for (UiDrawCommand& command : commands)
            if (!command.localizationKey.empty()) command.text = localizer_.resolve(command.localizationKey, data_);
        if (accessibility_.highContrast) {
            for (UiDrawCommand& command : commands) {
                const float luminance = 0.2126F * command.color.r + 0.7152F * command.color.g + 0.0722F * command.color.b;
                command.color = luminance > 0.5F ? UiColor{1,1,1,1} : UiColor{0,0,0,1};
            }
        }
        for (UiDrawCommand& command : commands) command.textScale = accessibility_.textScale;
        drawCommands_.insert(drawCommands_.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
    }
    std::stable_sort(drawCommands_.begin(), drawCommands_.end(), [](const UiDrawCommand& a, const UiDrawCommand& b) {
        return a.layer < b.layer;
    });
    build_render_primitives();
}

void UiRuntime::build_render_primitives() {
    renderPrimitives_.clear();
    auto add = [&](const UiDrawCommand& command, UiRenderPrimitiveKind kind, UiRect rectangle,
                   UiColor color, std::string text = {}, std::string image = {}) {
        UiRenderPrimitive primitive;
        primitive.canvas = command.canvas;
        primitive.widget = command.widget;
        primitive.kind = kind;
        primitive.rectangle = rectangle;
        primitive.clipRectangle = command.clipRectangle;
        primitive.color = color;
        primitive.text = std::move(text);
        primitive.imageAsset = std::move(image);
        primitive.canvasMode = command.canvasMode;
        primitive.worldTransform = command.worldTransform;
        primitive.layer = command.layer;
        primitive.textScale = command.textScale;
        renderPrimitives_.push_back(std::move(primitive));
    };
    for (const UiDrawCommand& command : drawCommands_) {
        UiColor color = command.color;
        if (!command.enabled) color.a *= 0.45F;
        switch (command.kind) {
            case UiWidgetKind::Text:
                add(command, UiRenderPrimitiveKind::Text, command.rectangle, color, command.text);
                break;
            case UiWidgetKind::Image:
                add(command, UiRenderPrimitiveKind::Image, command.rectangle, color, {}, command.imageAsset);
                break;
            case UiWidgetKind::ProgressBar: {
                add(command, UiRenderPrimitiveKind::Quad, command.rectangle, UiColor{0.08F, 0.08F, 0.08F, color.a});
                double normalized{};
                const auto canvas = canvases_.find(command.canvas);
                const UiWidget* widget = canvas == canvases_.end() ? nullptr : canvas->second.document.widget(command.widget);
                if (widget && widget->maximum > widget->minimum)
                    normalized = (command.value - widget->minimum) / (widget->maximum - widget->minimum);
                UiRect fill = command.rectangle;
                fill.width *= static_cast<float>(std::clamp(normalized, 0.0, 1.0));
                add(command, UiRenderPrimitiveKind::ProgressFill, fill, color);
                break;
            }
            case UiWidgetKind::Slider: {
                UiRect track = command.rectangle;
                track.y += track.height * 0.45F;
                track.height *= 0.1F;
                add(command, UiRenderPrimitiveKind::SliderTrack, track, color);
                double normalized{};
                const auto canvas = canvases_.find(command.canvas);
                const UiWidget* widget = canvas == canvases_.end() ? nullptr : canvas->second.document.widget(command.widget);
                if (widget && widget->maximum > widget->minimum)
                    normalized = (command.value - widget->minimum) / (widget->maximum - widget->minimum);
                UiRect thumb{command.rectangle.x + static_cast<float>(std::clamp(normalized, 0.0, 1.0)) * command.rectangle.width - 6.0F,
                             command.rectangle.y, 12.0F, command.rectangle.height};
                add(command, UiRenderPrimitiveKind::SliderThumb, thumb, color);
                break;
            }
            case UiWidgetKind::Spacer:
                break;
            default:
                add(command, UiRenderPrimitiveKind::Quad, command.rectangle, color);
                if (!command.text.empty())
                    add(command, UiRenderPrimitiveKind::Text, command.rectangle, UiColor{1,1,1,color.a}, command.text);
                if (command.kind == UiWidgetKind::TextInput && command.focused) {
                    const auto canvas = canvases_.find(command.canvas);
                    const UiWidget* widget = canvas == canvases_.end()
                        ? nullptr : canvas->second.document.widget(command.widget);
                    std::size_t caretByte = widget ? widget->text.size() : 0U;
                    if (canvas != canvases_.end()) {
                        const auto caret = canvas->second.textCaretBytes.find(command.widget);
                        if (caret != canvas->second.textCaretBytes.end()) caretByte = caret->second;
                    }
                    const std::size_t character = widget
                        ? utf8_codepoint_count(widget->text, caretByte) : 0U;
                    const float caretX = std::min(
                        command.rectangle.x + command.rectangle.width - 3.0F,
                        command.rectangle.x + 4.0F +
                            static_cast<float>(character) * 8.0F * command.textScale);
                    add(command, UiRenderPrimitiveKind::TextCaret,
                        {caretX, command.rectangle.y + 4.0F, 1.5F,
                         std::max(2.0F, command.rectangle.height - 8.0F)},
                        UiColor{1.0F, 1.0F, 1.0F, color.a});
                }
                break;
        }
        if (command.focused)
            add(command, UiRenderPrimitiveKind::FocusRing, command.rectangle, UiColor{1.0F, 0.82F, 0.12F, 1.0F});
    }
}

UiWidgetId UiRuntime::focused_widget(UiCanvasId id) const noexcept {
    const auto found = canvases_.find(id);
    return found == canvases_.end() ? kInvalidUiWidgetId : found->second.focused;
}

void UiRuntime::set_accessibility(UiAccessibilityStyle style) noexcept {
    if (!std::isfinite(style.textScale)) style.textScale = 1.0F;
    style.textScale = std::clamp(style.textScale, 0.5F, 4.0F);
    accessibility_ = style;
}

UiDocument make_default_gameplay_hud() {
    UiDocument document;
    UiWidget* root = document.widget(document.root());
    root->layout.direction = UiLayoutDirection::Absolute;
    std::string ignored;
    const UiWidgetId health = document.add_widget(document.root(), UiWidgetKind::ProgressBar, "Health", &ignored);
    UiWidget* healthWidget = document.widget(health);
    healthWidget->layout.position = {32.0F, 32.0F};
    healthWidget->layout.preferredSize = {320.0F, 28.0F};
    healthWidget->binding = "player.health";
    healthWidget->bindingTarget = UiBindingTarget::Value;
    healthWidget->color = {0.8F, 0.08F, 0.05F, 0.95F};
    const UiWidgetId prompt = document.add_widget(document.root(), UiWidgetKind::Text, "InteractionPrompt", &ignored);
    UiWidget* promptWidget = document.widget(prompt);
    promptWidget->layout.position = {710.0F, 880.0F};
    promptWidget->layout.preferredSize = {500.0F, 48.0F};
    promptWidget->binding = "hud.interaction";
    promptWidget->bindingTarget = UiBindingTarget::Text;
    promptWidget->color = {1.0F, 1.0F, 1.0F, 1.0F};
    return document;
}

} // namespace dve::ui
