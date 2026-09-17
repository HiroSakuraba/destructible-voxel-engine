#include "dve/platform/headless_application_host.hpp"

#include <thread>
#include <utility>

namespace dve::platform {

HeadlessApplicationHost::HeadlessApplicationHost() : epoch_(std::chrono::steady_clock::now()) {}

bool HeadlessApplicationHost::create_window(const WindowDesc& desc, std::string* error) {
    if (desc.width <= 0 || desc.height <= 0) {
        if (error != nullptr) *error = "window dimensions must be positive";
        return false;
    }
    hasWindow_ = true;
    title_ = desc.title;
    metrics_.logicalWidth = desc.width;
    metrics_.logicalHeight = desc.height;
    metrics_.drawableWidth = desc.width;
    metrics_.drawableHeight = desc.height;
    metrics_.contentScale = 1.0F;
    metrics_.focused = !desc.hidden;
    metrics_.minimized = false;
    return true;
}

void HeadlessApplicationHost::destroy_window() noexcept {
    hasWindow_ = false;
    metrics_ = {};
    events_.clear();
}

bool HeadlessApplicationHost::poll_event(PlatformEvent& event) {
    if (events_.empty()) return false;
    event = std::move(events_.front());
    events_.pop_front();
    if (event.type == EventType::WindowResized) {
        metrics_.logicalWidth = event.width;
        metrics_.logicalHeight = event.height;
        metrics_.drawableWidth = event.width;
        metrics_.drawableHeight = event.height;
    } else if (event.type == EventType::WindowFocusGained) {
        metrics_.focused = true;
    } else if (event.type == EventType::WindowFocusLost) {
        metrics_.focused = false;
    }
    return true;
}

NativeWindowHandle HeadlessApplicationHost::native_window_handle() const noexcept {
    return {HostBackend::Headless, nullptr, nullptr, 0U};
}

void HeadlessApplicationHost::set_window_title(std::string_view title) {
    title_.assign(title.begin(), title.end());
}

void HeadlessApplicationHost::set_clipboard_text(std::string_view text) {
    clipboard_.assign(text.begin(), text.end());
}

FileDialogToken HeadlessApplicationHost::request_file_dialog(const FileDialogRequest& request) {
    const FileDialogToken token = nextDialogToken_++;
    pendingDialogs_.emplace(token, request);
    return token;
}

std::optional<FileDialogResult> HeadlessApplicationHost::take_file_dialog_result(FileDialogToken token) {
    const auto found = completedDialogs_.find(token);
    if (found == completedDialogs_.end()) return std::nullopt;
    FileDialogResult result = std::move(found->second);
    completedDialogs_.erase(found);
    return result;
}

double HeadlessApplicationHost::monotonic_seconds() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_).count();
}

void HeadlessApplicationHost::sleep_for(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
}

void HeadlessApplicationHost::push_event(PlatformEvent event) {
    if (event.timestampNanoseconds == 0U) {
        event.timestampNanoseconds = static_cast<std::uint64_t>(monotonic_seconds() * 1'000'000'000.0);
    }
    events_.push_back(std::move(event));
}

bool HeadlessApplicationHost::complete_file_dialog(FileDialogToken token,
                                                   std::vector<std::string> paths,
                                                   std::string error) {
    const auto pending = pendingDialogs_.find(token);
    if (pending == pendingDialogs_.end()) return false;
    FileDialogResult result;
    result.token = token;
    result.completed = true;
    result.accepted = error.empty() && !paths.empty();
    result.paths = std::move(paths);
    result.error = std::move(error);
    completedDialogs_[token] = std::move(result);
    pendingDialogs_.erase(pending);
    return true;
}

} // namespace dve::platform
