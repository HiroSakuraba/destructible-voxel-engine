#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace dve::editor {

using EditorTaskId = std::uint64_t;
enum class EditorTaskState : std::uint8_t { Queued, Running, Succeeded, Failed, Cancelled };

struct EditorTaskSnapshot {
    EditorTaskId id{};
    std::string label;
    std::string phase;
    float progress{};
    EditorTaskState state{EditorTaskState::Queued};
    std::string error;
};

class EditorTaskContext {
public:
    [[nodiscard]] bool cancelled() const noexcept;
    void report(float progress, std::string phase);
private:
    friend class EditorTaskManager;
    struct Shared;
    explicit EditorTaskContext(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}
    std::shared_ptr<Shared> shared_;
};

class EditorTaskManager {
public:
    using Work = std::function<void(EditorTaskContext&)>;
    explicit EditorTaskManager(std::size_t workers = 2, std::size_t maximumQueued = 64);
    ~EditorTaskManager();
    EditorTaskManager(const EditorTaskManager&) = delete;
    EditorTaskManager& operator=(const EditorTaskManager&) = delete;

    [[nodiscard]] std::optional<EditorTaskId> submit(std::string label, Work work,
                                                     std::string* error = nullptr);
    [[nodiscard]] bool cancel(EditorTaskId id);
    [[nodiscard]] std::optional<EditorTaskSnapshot> snapshot(EditorTaskId id) const;
    [[nodiscard]] std::vector<EditorTaskSnapshot> snapshots() const;
    void wait_idle();
private:
    struct Job;
    void worker_loop();
    std::size_t maximumQueued_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idleCondition_;
    std::queue<std::shared_ptr<Job>> queue_;
    std::map<EditorTaskId, std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> workers_;
    EditorTaskId nextId_{1};
    std::size_t running_{};
    bool stopping_{};
};

} // namespace dve::editor
