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
    [[nodiscard]] bool poll_event(PlatformEvent& event) override;
    [[nodiscard]] WindowMetrics window_metrics() const noexcept override;
    [[nodiscard]] NativeWindowHandle native_window_handle() const noexcept override;
    void set_window_title(std::string_view title) override;

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
