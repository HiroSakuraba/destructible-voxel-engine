#include "dve/platform/sdl_application_host.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace dve::platform {
namespace {

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

Modifier modifiers_from_sdl(SDL_Keymod value) noexcept {
    Modifier result = Modifier::NoModifiers;
    if ((value & SDL_KMOD_SHIFT) != 0) result = result | Modifier::Shift;
    if ((value & SDL_KMOD_CTRL) != 0) result = result | Modifier::Control;
    if ((value & SDL_KMOD_ALT) != 0) result = result | Modifier::Alt;
    if ((value & SDL_KMOD_GUI) != 0) result = result | Modifier::Super;
    if ((value & SDL_KMOD_CAPS) != 0) result = result | Modifier::CapsLock;
    if ((value & SDL_KMOD_NUM) != 0) result = result | Modifier::NumLock;
    return result;
}

PointerButton pointer_button_from_sdl(Uint8 button) noexcept {
    switch (button) {
        case SDL_BUTTON_LEFT: return PointerButton::Primary;
        case SDL_BUTTON_MIDDLE: return PointerButton::Auxiliary;
        case SDL_BUTTON_RIGHT: return PointerButton::Secondary;
        case SDL_BUTTON_X1: return PointerButton::Extra1;
        case SDL_BUTTON_X2: return PointerButton::Extra2;
        default: return PointerButton::NoButton;
    }
}

GamepadButton gamepad_button_from_sdl(Uint8 button) noexcept {
    switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return GamepadButton::South;
        case SDL_GAMEPAD_BUTTON_EAST: return GamepadButton::East;
        case SDL_GAMEPAD_BUTTON_WEST: return GamepadButton::West;
        case SDL_GAMEPAD_BUTTON_NORTH: return GamepadButton::North;
        case SDL_GAMEPAD_BUTTON_BACK: return GamepadButton::Back;
        case SDL_GAMEPAD_BUTTON_GUIDE: return GamepadButton::Guide;
        case SDL_GAMEPAD_BUTTON_START: return GamepadButton::Start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return GamepadButton::LeftStick;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return GamepadButton::RightStick;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return GamepadButton::LeftShoulder;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GamepadButton::RightShoulder;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return GamepadButton::DpadUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return GamepadButton::DpadDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return GamepadButton::DpadLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return GamepadButton::DpadRight;
        case SDL_GAMEPAD_BUTTON_MISC1: return GamepadButton::Misc1;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return GamepadButton::RightPaddle1;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return GamepadButton::LeftPaddle1;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return GamepadButton::RightPaddle2;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return GamepadButton::LeftPaddle2;
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return GamepadButton::Touchpad;
        case SDL_GAMEPAD_BUTTON_MISC2: return GamepadButton::Misc2;
        case SDL_GAMEPAD_BUTTON_MISC3: return GamepadButton::Misc3;
        case SDL_GAMEPAD_BUTTON_MISC4: return GamepadButton::Misc4;
        case SDL_GAMEPAD_BUTTON_MISC5: return GamepadButton::Misc5;
        case SDL_GAMEPAD_BUTTON_MISC6: return GamepadButton::Misc6;
        default: return GamepadButton::Unknown;
    }
}

GamepadAxis gamepad_axis_from_sdl(Uint8 axis) noexcept {
    switch (static_cast<SDL_GamepadAxis>(axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX: return GamepadAxis::LeftX;
        case SDL_GAMEPAD_AXIS_LEFTY: return GamepadAxis::LeftY;
        case SDL_GAMEPAD_AXIS_RIGHTX: return GamepadAxis::RightX;
        case SDL_GAMEPAD_AXIS_RIGHTY: return GamepadAxis::RightY;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return GamepadAxis::LeftTrigger;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return GamepadAxis::RightTrigger;
        default: return GamepadAxis::Unknown;
    }
}

float normalized_gamepad_axis(Uint8 axis, Sint16 value) noexcept {
    const auto mapped = gamepad_axis_from_sdl(axis);
    if (mapped == GamepadAxis::LeftTrigger || mapped == GamepadAxis::RightTrigger)
        return std::clamp(static_cast<float>(value) / 32767.0F, 0.0F, 1.0F);
    const float divisor = value < 0 ? 32768.0F : 32767.0F;
    return std::clamp(static_cast<float>(value) / divisor, -1.0F, 1.0F);
}

std::string normalized_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_ESCAPE: return "escape";
        case SDLK_TAB: return "tab";
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return "return";
        case SDLK_BACKSPACE: return "backspace";
        case SDLK_DELETE: return "delete";
        case SDLK_SPACE: return "space";
        case SDLK_LEFT: return "left";
        case SDLK_RIGHT: return "right";
        case SDLK_UP: return "up";
        case SDLK_DOWN: return "down";
        case SDLK_F1: return "f1";
        case SDLK_F2: return "f2";
        case SDLK_F5: return "f5";
        case SDLK_F6: return "f6";
        case SDLK_EQUALS: return "equal";
        case SDLK_PLUS: return "+";
        case SDLK_MINUS: return "minus";
        case SDLK_LEFTBRACKET: return "[";
        case SDLK_RIGHTBRACKET: return "]";
        default: break;
    }
    const char* name = SDL_GetKeyName(key);
    std::string result = name != nullptr ? name : "";
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

} // namespace

struct SdlApplicationHost::Impl {
    struct PendingDialog {
        FileDialogToken token{};
        std::vector<std::string> names;
        std::vector<std::string> patterns;
        std::vector<SDL_DialogFileFilter> filters;
        std::string defaultLocation;
    };

    SDL_Window* window{};
    bool initialized{};
    FileDialogToken nextDialogToken{1};
    mutable std::mutex dialogMutex;
    std::unordered_map<FileDialogToken, std::unique_ptr<PendingDialog>> pendingDialogs;
    std::unordered_map<FileDialogToken, FileDialogResult> completedDialogs;
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads;

    static void SDLCALL dialog_callback(void* userdata, const char* const* filelist, int) {
        auto* pending = static_cast<PendingDialog*>(userdata);
        if (pending == nullptr) return;
        // The owner remains alive until this callback publishes the result.
        // Locate it through the token-to-pending map stored by the enclosing host.
        // owner is encoded in the final filter pointer only in test shims? No: use registry.
        publish_dialog_result(pending, filelist);
    }

    static std::mutex registryMutex;
    static std::unordered_map<PendingDialog*, Impl*> dialogOwners;

    static void publish_dialog_result(PendingDialog* pending, const char* const* filelist) {
        Impl* owner = nullptr;
        {
            std::scoped_lock lock(registryMutex);
            const auto found = dialogOwners.find(pending);
            if (found != dialogOwners.end()) owner = found->second;
        }
        if (owner == nullptr) return;

        FileDialogResult result;
        result.token = pending->token;
        result.completed = true;
        if (filelist == nullptr) {
            const char* message = SDL_GetError();
            result.error = message != nullptr ? message : "SDL file dialog failed";
        } else {
            for (const char* const* path = filelist; *path != nullptr; ++path)
                result.paths.emplace_back(*path);
            result.accepted = !result.paths.empty();
        }

        std::scoped_lock lock(owner->dialogMutex, registryMutex);
        owner->completedDialogs[result.token] = std::move(result);
        dialogOwners.erase(pending);
        owner->pendingDialogs.erase(pending->token);
    }

    [[nodiscard]] WindowMetrics metrics() const noexcept {
        WindowMetrics result{};
        if (window == nullptr) return result;
        (void)SDL_GetWindowSize(window, &result.logicalWidth, &result.logicalHeight);
        (void)SDL_GetWindowSizeInPixels(window, &result.drawableWidth, &result.drawableHeight);
        const float scale = SDL_GetWindowDisplayScale(window);
        result.contentScale = scale > 0.0F ? scale : 1.0F;
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        result.focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
        result.minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
        return result;
    }
};

std::mutex SdlApplicationHost::Impl::registryMutex;
std::unordered_map<SdlApplicationHost::Impl::PendingDialog*, SdlApplicationHost::Impl*>
    SdlApplicationHost::Impl::dialogOwners;

SdlApplicationHost::SdlApplicationHost() : impl_(std::make_unique<Impl>()) {}
SdlApplicationHost::~SdlApplicationHost() { destroy_window(); }
SdlApplicationHost::SdlApplicationHost(SdlApplicationHost&&) noexcept = default;
SdlApplicationHost& SdlApplicationHost::operator=(SdlApplicationHost&&) noexcept = default;

bool SdlApplicationHost::create_window(const WindowDesc& desc, std::string* error) {
    if (desc.width <= 0 || desc.height <= 0) {
        set_error(error, "window dimensions must be positive");
        return false;
    }
    destroy_window();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        set_error(error, SDL_GetError());
        return false;
    }
    impl_->initialized = true;
    SDL_WindowFlags flags = 0;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.highDpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
    impl_->window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (impl_->window == nullptr) {
        set_error(error, SDL_GetError());
        destroy_window();
        return false;
    }
    if (!desc.hidden) (void)SDL_ShowWindow(impl_->window);
    if (!SDL_StartTextInput(impl_->window)) {
        set_error(error, SDL_GetError());
        destroy_window();
        return false;
    }
    return true;
}

void SdlApplicationHost::destroy_window() noexcept {
    if (!impl_) return;
    {
        std::scoped_lock lock(impl_->dialogMutex, Impl::registryMutex);
        for (const auto& [token, dialog] : impl_->pendingDialogs) {
            (void)token;
            Impl::dialogOwners.erase(dialog.get());
        }
        impl_->pendingDialogs.clear();
        impl_->completedDialogs.clear();
    }
    for (auto& [id, gamepad] : impl_->gamepads) {
        (void)id;
        if (gamepad != nullptr) SDL_CloseGamepad(gamepad);
    }
    impl_->gamepads.clear();
    if (impl_->window != nullptr) {
        SDL_StopTextInput(impl_->window);
        SDL_DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    if (impl_->initialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
        impl_->initialized = false;
    }
}

bool SdlApplicationHost::has_window() const noexcept { return impl_ && impl_->window != nullptr; }

bool SdlApplicationHost::poll_event(PlatformEvent& output) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        PlatformEvent converted;
        converted.timestampNanoseconds = event.common.timestamp;
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                converted.type = EventType::QuitRequested;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                converted.type = EventType::WindowResized;
                const WindowMetrics current = impl_->metrics();
                converted.width = current.logicalWidth;
                converted.height = current.logicalHeight;
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_GAINED: converted.type = EventType::WindowFocusGained; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: converted.type = EventType::WindowFocusLost; break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                converted.type = event.type == SDL_EVENT_KEY_DOWN ? EventType::KeyDown : EventType::KeyUp;
                converted.key = normalized_key(event.key.key);
                converted.modifiers = modifiers_from_sdl(event.key.mod);
                converted.repeat = event.key.repeat;
                break;
            case SDL_EVENT_TEXT_EDITING:
                converted.type = EventType::TextEditing;
                if (event.edit.text != nullptr) converted.text = event.edit.text;
                converted.compositionStart = event.edit.start;
                converted.compositionLength = event.edit.length;
                break;
            case SDL_EVENT_TEXT_INPUT:
                converted.type = EventType::TextInput;
                if (event.text.text != nullptr) converted.text = event.text.text;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                converted.type = EventType::PointerMove;
                converted.x = static_cast<int>(std::lround(event.motion.x));
                converted.y = static_cast<int>(std::lround(event.motion.y));
                converted.modifiers = modifiers_from_sdl(SDL_GetModState());
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                converted.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                     ? EventType::PointerButtonDown : EventType::PointerButtonUp;
                converted.x = static_cast<int>(std::lround(event.button.x));
                converted.y = static_cast<int>(std::lround(event.button.y));
                converted.button = pointer_button_from_sdl(event.button.button);
                converted.modifiers = modifiers_from_sdl(SDL_GetModState());
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                converted.type = EventType::PointerWheel;
                converted.wheelX = event.wheel.x;
                converted.wheelY = event.wheel.y;
                converted.modifiers = modifiers_from_sdl(SDL_GetModState());
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                converted.type = EventType::GamepadAdded;
                converted.gamepadId = event.gdevice.which;
                if (!impl_->gamepads.contains(event.gdevice.which)) {
                    if (SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which); gamepad != nullptr)
                        impl_->gamepads.emplace(event.gdevice.which, gamepad);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                converted.type = EventType::GamepadRemoved;
                converted.gamepadId = event.gdevice.which;
                const auto found = impl_->gamepads.find(event.gdevice.which);
                if (found != impl_->gamepads.end()) {
                    SDL_CloseGamepad(found->second);
                    impl_->gamepads.erase(found);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                converted.type = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
                                     ? EventType::GamepadButtonDown : EventType::GamepadButtonUp;
                converted.gamepadId = event.gbutton.which;
                converted.gamepadButton = gamepad_button_from_sdl(event.gbutton.button);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                converted.type = EventType::GamepadAxisMotion;
                converted.gamepadId = event.gaxis.which;
                converted.gamepadAxis = gamepad_axis_from_sdl(event.gaxis.axis);
                converted.gamepadValue = normalized_gamepad_axis(event.gaxis.axis, event.gaxis.value);
                break;
            case SDL_EVENT_DROP_FILE:
                converted.type = EventType::FileDropped;
                if (event.drop.data != nullptr) converted.path = event.drop.data;
                break;
            default:
                continue;
        }
        output = std::move(converted);
        return true;
    }
    return false;
}

WindowMetrics SdlApplicationHost::window_metrics() const noexcept {
    return impl_ ? impl_->metrics() : WindowMetrics{};
}

NativeWindowHandle SdlApplicationHost::native_window_handle() const noexcept {
    return {HostBackend::SDL3, impl_ ? impl_->window : nullptr, nullptr,
            impl_ && impl_->window ? static_cast<std::uintptr_t>(SDL_GetWindowID(impl_->window)) : 0U};
}

void SdlApplicationHost::set_window_title(std::string_view title) {
    if (!has_window()) return;
    const std::string owned(title);
    (void)SDL_SetWindowTitle(impl_->window, owned.c_str());
}

void SdlApplicationHost::set_clipboard_text(std::string_view text) {
    const std::string owned(text);
    (void)SDL_SetClipboardText(owned.c_str());
}

std::string SdlApplicationHost::clipboard_text() const {
    char* text = SDL_GetClipboardText();
    if (text == nullptr) return {};
    std::string result(text);
    SDL_free(text);
    return result;
}

FileDialogToken SdlApplicationHost::request_file_dialog(const FileDialogRequest& request) {
    auto pending = std::make_unique<Impl::PendingDialog>();
    pending->token = impl_->nextDialogToken++;
    pending->defaultLocation = request.initialPath;
    pending->names.reserve(request.filters.size());
    pending->patterns.reserve(request.filters.size());
    pending->filters.reserve(request.filters.size());
    for (const FileDialogFilter& filter : request.filters) {
        pending->names.push_back(filter.name);
        std::string pattern;
        for (std::size_t index = 0; index < filter.extensions.size(); ++index) {
            if (index != 0U) pattern.push_back(';');
            pattern += filter.extensions[index];
        }
        if (pattern.empty()) pattern = "*";
        pending->patterns.push_back(std::move(pattern));
    }
    for (std::size_t index = 0; index < pending->names.size(); ++index)
        pending->filters.push_back({pending->names[index].c_str(), pending->patterns[index].c_str()});

    const FileDialogToken token = pending->token;
    Impl::PendingDialog* raw = pending.get();
    {
        std::scoped_lock lock(impl_->dialogMutex, Impl::registryMutex);
        impl_->pendingDialogs[token] = std::move(pending);
        Impl::dialogOwners[raw] = impl_.get();
    }

    const SDL_DialogFileFilter* filters = raw->filters.empty() ? nullptr : raw->filters.data();
    const int filterCount = static_cast<int>(raw->filters.size());
    const char* location = raw->defaultLocation.empty() ? nullptr : raw->defaultLocation.c_str();
    switch (request.kind) {
        case FileDialogKind::OpenFile:
            SDL_ShowOpenFileDialog(&Impl::dialog_callback, raw, impl_->window, filters, filterCount,
                                   location, request.allowMultiple);
            break;
        case FileDialogKind::SaveFile:
            SDL_ShowSaveFileDialog(&Impl::dialog_callback, raw, impl_->window, filters, filterCount,
                                   location);
            break;
        case FileDialogKind::SelectFolder:
            SDL_ShowOpenFolderDialog(&Impl::dialog_callback, raw, impl_->window, location,
                                     request.allowMultiple);
            break;
    }
    return token;
}

std::optional<FileDialogResult> SdlApplicationHost::take_file_dialog_result(FileDialogToken token) {
    std::scoped_lock lock(impl_->dialogMutex);
    const auto found = impl_->completedDialogs.find(token);
    if (found == impl_->completedDialogs.end()) return std::nullopt;
    FileDialogResult result = std::move(found->second);
    impl_->completedDialogs.erase(found);
    return result;
}

double SdlApplicationHost::monotonic_seconds() const noexcept {
    return static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0;
}

void SdlApplicationHost::sleep_for(std::chrono::milliseconds duration) {
    if (duration.count() > 0) SDL_Delay(static_cast<Uint32>(duration.count()));
}

} // namespace dve::platform
