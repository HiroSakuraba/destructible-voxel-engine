#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dve::platform {

enum class HostBackend : std::uint8_t {
    Headless,
    SDL3,
    Win32,
    X11,
    Cocoa,
};

enum class EventType : std::uint8_t {
    NoEvent,
    QuitRequested,
    WindowResized,
    WindowFocusGained,
    WindowFocusLost,
    KeyDown,
    KeyUp,
    TextInput,
    TextEditing,
    PointerMove,
    PointerButtonDown,
    PointerButtonUp,
    PointerWheel,
    FileDropped,
    GamepadAdded,
    GamepadRemoved,
    GamepadButtonDown,
    GamepadButtonUp,
    GamepadAxisMotion,
};

enum class PointerButton : std::uint8_t {
    NoButton,
    Primary,
    Auxiliary,
    Secondary,
    Extra1,
    Extra2,
};

enum class GamepadButton : std::uint8_t {
    Unknown, South, East, West, North, Back, Guide, Start, LeftStick, RightStick,
    LeftShoulder, RightShoulder, DpadUp, DpadDown, DpadLeft, DpadRight, Misc1,
    RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2, Touchpad, Misc2, Misc3,
    Misc4, Misc5, Misc6,
};

enum class GamepadAxis : std::uint8_t {
    Unknown, LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger,
};

enum class Modifier : std::uint32_t {
    NoModifiers = 0,
    Shift = 1U << 0U,
    Control = 1U << 1U,
    Alt = 1U << 2U,
    Super = 1U << 3U,
    CapsLock = 1U << 4U,
    NumLock = 1U << 5U,
};

[[nodiscard]] constexpr Modifier operator|(Modifier left, Modifier right) noexcept {
    return static_cast<Modifier>(static_cast<std::uint32_t>(left) |
                                 static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr Modifier operator&(Modifier left, Modifier right) noexcept {
    return static_cast<Modifier>(static_cast<std::uint32_t>(left) &
                                 static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr bool has_modifier(Modifier value, Modifier flag) noexcept {
    return (value & flag) != Modifier::NoModifiers;
}

struct WindowDesc {
    std::string title{"Destructible Voxel Engine"};
    int width{1280};
    int height{800};
    bool resizable{true};
    bool highDpi{true};
    bool hidden{};
};

struct WindowMetrics {
    int logicalWidth{};
    int logicalHeight{};
    int drawableWidth{};
    int drawableHeight{};
    float contentScale{1.0F};
    bool focused{true};
    bool minimized{};
};

// Opaque transport for renderer backends. The platform implementation owns the pointed-to
// objects and documents which fields are meaningful for its HostBackend. No platform header
// is required by callers that merely forward the handle into an RHI swapchain factory.
struct NativeWindowHandle {
    HostBackend backend{HostBackend::Headless};
    void* window{};
    void* display{};
    std::uintptr_t auxiliary{};
};

struct PlatformEvent {
    EventType type{EventType::NoEvent};
    std::uint64_t timestampNanoseconds{};
    Modifier modifiers{Modifier::NoModifiers};
    PointerButton button{PointerButton::NoButton};
    int x{};
    int y{};
    int width{};
    int height{};
    float wheelX{};
    float wheelY{};
    bool repeat{};
    std::int32_t compositionStart{-1};
    std::int32_t compositionLength{-1};
    std::int32_t gamepadId{-1};
    GamepadButton gamepadButton{GamepadButton::Unknown};
    GamepadAxis gamepadAxis{GamepadAxis::Unknown};
    float gamepadValue{};
    std::string key;
    std::string text;
    std::string path;
};

enum class FileDialogKind : std::uint8_t { OpenFile, SaveFile, SelectFolder };

struct FileDialogFilter {
    std::string name;
    std::vector<std::string> extensions;
};

struct FileDialogRequest {
    FileDialogKind kind{FileDialogKind::OpenFile};
    std::string title;
    std::string initialPath;
    std::string suggestedName;
    std::vector<FileDialogFilter> filters;
    bool allowMultiple{};
};

using FileDialogToken = std::uint64_t;

struct FileDialogResult {
    FileDialogToken token{};
    bool completed{};
    bool accepted{};
    std::vector<std::string> paths;
    std::string error;
};

class IApplicationHost {
public:
    virtual ~IApplicationHost() = default;

    [[nodiscard]] virtual HostBackend backend() const noexcept = 0;
    virtual bool create_window(const WindowDesc& desc, std::string* error = nullptr) = 0;
    virtual void destroy_window() noexcept = 0;
    [[nodiscard]] virtual bool has_window() const noexcept = 0;
    [[nodiscard]] virtual bool poll_event(PlatformEvent& event) = 0;
    [[nodiscard]] virtual WindowMetrics window_metrics() const noexcept = 0;
    [[nodiscard]] virtual NativeWindowHandle native_window_handle() const noexcept = 0;
    virtual void set_window_title(std::string_view title) = 0;

    virtual void set_clipboard_text(std::string_view text) = 0;
    [[nodiscard]] virtual std::string clipboard_text() const = 0;

    [[nodiscard]] virtual FileDialogToken request_file_dialog(const FileDialogRequest& request) = 0;
    [[nodiscard]] virtual std::optional<FileDialogResult> take_file_dialog_result(FileDialogToken token) = 0;

    [[nodiscard]] virtual double monotonic_seconds() const noexcept = 0;
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
};

[[nodiscard]] std::string_view host_backend_name(HostBackend backend) noexcept;

} // namespace dve::platform
