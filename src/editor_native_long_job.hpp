#pragma once

// The background job record of NativeEditorController (start_long_job). Private to the editor's
// source files: included by editor_native.cpp and the feature files that run long jobs.

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "dve/editor_native.hpp"

namespace dve::editor {

struct NativeEditorController::LongJob {
    LongJobKind kind{LongJobKind::Idle};
    std::string label;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<float> progress{0.0F};
    mutable std::mutex mutex;
    std::string phase;
    std::string error;
    std::uint64_t documentRevision{};
    std::function<bool(LongJob&)> commit;
    void report(float value, std::string text) {
        progress.store(std::clamp(value, 0.0F, 1.0F));
        std::lock_guard lock(mutex);
        phase = std::move(text);
    }
    [[nodiscard]] bool cancelled() const noexcept { return cancel.load(); }
};

}  // namespace dve::editor
