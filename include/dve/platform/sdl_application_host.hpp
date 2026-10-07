#pragma once

#include <memory>

#include "dve/platform/application_host.hpp"

namespace dve::platform {

// SDL3 implementation of the platform contract. SDL types are hidden behind a pimpl so
// platform-neutral users of dve::platform never include SDL headers.
class SdlApplicationHost final : public IApplicationHost {
public:
    SdlApplicationHost();
    ~SdlApplicationHost() override;

    SdlApplicationHost(const SdlApplicationHost&) = delete;
    SdlApplicationHost& operator=(const SdlApplicationHost&) = delete;
    SdlApplicationHost(SdlApplicationHost&&) noexcept;
    SdlApplicationHost& operator=(SdlApplicationHost&&) noexcept;

    [[nodiscard]] HostBackend backend() const noexcept override { return HostBackend::SDL3; }
    bool create_window(const WindowDesc& desc, std::string* error = nullptr) override;
    void destroy_window() noexcept override;
    [[nodiscard]] bool has_window() const noexcept override;
    // create_window initializes SDL audio separately from video (WindowDesc::initializeAudio)
    // and never fails because of it; these report whether that succeeded and why not.
    [[nodiscard]] bool audio_subsystem_initialized() const noexcept;
    [[nodiscard]] std::string audio_init_error() const;
    [[nodiscard]] bool poll_event(PlatformEvent& event) override;
    // Wait on the video thread without consuming an event; poll_event() still
    // handles conversion and delivers the queued event on the next frame.
    [[nodiscard]] bool wait_for_events(std::chrono::milliseconds timeout);
    // Include update/render/present work in the budget, but keep a minimum frame
    // interval even when mouse motion continuously wakes the event wait.
    [[nodiscard]] bool wait_for_frame(double frameStart, std::chrono::milliseconds targetInterval,
                                     std::chrono::milliseconds minimumInterval);
    [[nodiscard]] WindowMetrics window_metrics() const noexcept override;
    [[nodiscard]] NativeWindowHandle native_window_handle() const noexcept override;
    void set_window_title(std::string_view title) override;
    bool set_relative_mouse_mode(bool enabled, std::string* error = nullptr) override;
    [[nodiscard]] bool relative_mouse_mode() const noexcept override;

    void set_clipboard_text(std::string_view text) override;
    [[nodiscard]] std::string clipboard_text() const override;

    [[nodiscard]] FileDialogToken request_file_dialog(const FileDialogRequest& request) override;
    [[nodiscard]] std::optional<FileDialogResult> take_file_dialog_result(FileDialogToken token) override;

    [[nodiscard]] double monotonic_seconds() const noexcept override;
    void sleep_for(std::chrono::milliseconds duration) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::platform
