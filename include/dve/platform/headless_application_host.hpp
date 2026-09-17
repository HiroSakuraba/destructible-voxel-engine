#pragma once

#include <deque>
#include <unordered_map>

#include "dve/platform/application_host.hpp"

namespace dve::platform {

// Deterministic dependency-free host used by unit tests, server tools, and CI. It exercises
// the same event and file-dialog contract as desktop hosts without claiming to create a real
// OS window.
class HeadlessApplicationHost final : public IApplicationHost {
public:
    HeadlessApplicationHost();

    [[nodiscard]] HostBackend backend() const noexcept override { return HostBackend::Headless; }
    bool create_window(const WindowDesc& desc, std::string* error = nullptr) override;
    void destroy_window() noexcept override;
    [[nodiscard]] bool has_window() const noexcept override { return hasWindow_; }
    [[nodiscard]] bool poll_event(PlatformEvent& event) override;
    [[nodiscard]] WindowMetrics window_metrics() const noexcept override { return metrics_; }
    [[nodiscard]] NativeWindowHandle native_window_handle() const noexcept override;
    void set_window_title(std::string_view title) override;

    void set_clipboard_text(std::string_view text) override;
    [[nodiscard]] std::string clipboard_text() const override { return clipboard_; }

    [[nodiscard]] FileDialogToken request_file_dialog(const FileDialogRequest& request) override;
    [[nodiscard]] std::optional<FileDialogResult> take_file_dialog_result(FileDialogToken token) override;

    [[nodiscard]] double monotonic_seconds() const noexcept override;
    void sleep_for(std::chrono::milliseconds duration) override;

    void push_event(PlatformEvent event);
    bool complete_file_dialog(FileDialogToken token, std::vector<std::string> paths,
                              std::string error = {});
    [[nodiscard]] std::string_view window_title() const noexcept { return title_; }

private:
    std::chrono::steady_clock::time_point epoch_;
    bool hasWindow_{};
    WindowMetrics metrics_{};
    std::string title_;
    std::string clipboard_;
    std::deque<PlatformEvent> events_;
    FileDialogToken nextDialogToken_{1};
    std::unordered_map<FileDialogToken, FileDialogRequest> pendingDialogs_;
    std::unordered_map<FileDialogToken, FileDialogResult> completedDialogs_;
};

} // namespace dve::platform
