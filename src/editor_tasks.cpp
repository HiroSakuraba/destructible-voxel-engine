#include "dve/editor_tasks.hpp"

#include <algorithm>
#include <exception>

namespace dve::editor {

struct EditorTaskContext::Shared {
    mutable std::mutex mutex;
    std::atomic_bool cancelled{};
    std::string phase{"Queued"};
    float progress{};
};

struct EditorTaskManager::Job {
    EditorTaskId id{};
    std::string label;
    Work work;
    std::shared_ptr<EditorTaskContext::Shared> shared;
    EditorTaskState state{EditorTaskState::Queued};
    std::string error;
};

bool EditorTaskContext::cancelled() const noexcept { return shared_->cancelled.load(std::memory_order_relaxed); }
void EditorTaskContext::report(float progress, std::string phase) {
    std::lock_guard lock(shared_->mutex);
    shared_->progress = std::clamp(progress, 0.0F, 1.0F);
    shared_->phase = std::move(phase);
}

EditorTaskManager::EditorTaskManager(std::size_t workers, std::size_t maximumQueued)
    : maximumQueued_(std::max<std::size_t>(1, maximumQueued)) {
    workers = std::max<std::size_t>(1, workers);
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
}
EditorTaskManager::~EditorTaskManager() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [id, job] : jobs_) { (void)id; job->shared->cancelled.store(true, std::memory_order_relaxed); }
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
}

std::optional<EditorTaskId> EditorTaskManager::submit(std::string label, Work work, std::string* error) {
    if (!work) { if (error) *error = "task has no work function"; return std::nullopt; }
    std::lock_guard lock(mutex_);
    if (stopping_) { if (error) *error = "task manager is stopping"; return std::nullopt; }
    if (queue_.size() >= maximumQueued_) { if (error) *error = "editor task queue is full"; return std::nullopt; }
    auto job = std::make_shared<Job>();
    job->id = nextId_++;
    job->label = std::move(label);
    job->work = std::move(work);
    job->shared = std::make_shared<EditorTaskContext::Shared>();
    jobs_[job->id] = job;
    queue_.push(job);
    condition_.notify_one();
    return job->id;
}
bool EditorTaskManager::cancel(EditorTaskId id) {
    std::lock_guard lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) return false;
    it->second->shared->cancelled.store(true, std::memory_order_relaxed);
    return true;
}

std::optional<EditorTaskSnapshot> EditorTaskManager::snapshot(EditorTaskId id) const {
    std::lock_guard lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) return std::nullopt;
    const auto& job = *it->second;
    std::lock_guard sharedLock(job.shared->mutex);
    return EditorTaskSnapshot{job.id, job.label, job.shared->phase, job.shared->progress, job.state, job.error};
}
std::vector<EditorTaskSnapshot> EditorTaskManager::snapshots() const {
    std::vector<EditorTaskSnapshot> result;
    std::lock_guard lock(mutex_);
    result.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
        (void)id;
        std::lock_guard sharedLock(job->shared->mutex);
        result.push_back({job->id, job->label, job->shared->phase, job->shared->progress, job->state, job->error});
    }
    return result;
}
void EditorTaskManager::wait_idle() {
    std::unique_lock lock(mutex_);
    idleCondition_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
}
void EditorTaskManager::worker_loop() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = queue_.front();
            queue_.pop();
            ++running_;
            job->state = EditorTaskState::Running;
        }

        EditorTaskContext context(job->shared);
        EditorTaskState finalState = EditorTaskState::Succeeded;
        std::string finalError;
        try {
            if (context.cancelled()) finalState = EditorTaskState::Cancelled;
            else {
                job->work(context);
                finalState = context.cancelled() ? EditorTaskState::Cancelled : EditorTaskState::Succeeded;
            }
        } catch (const std::exception& exception) {
            finalError = exception.what();
            finalState = EditorTaskState::Failed;
        } catch (...) {
            finalError = "unknown task failure";
            finalState = EditorTaskState::Failed;
        }

        {
            // Keep the same lock order used by snapshot(): manager, then shared progress.
            std::lock_guard lock(mutex_);
            job->state = finalState;
            job->error = std::move(finalError);
            {
                std::lock_guard sharedLock(job->shared->mutex);
                if (finalState == EditorTaskState::Succeeded) {
                    job->shared->progress = 1.0F;
                    job->shared->phase = "Complete";
                } else if (finalState == EditorTaskState::Cancelled) {
                    job->shared->phase = "Cancelled";
                } else if (finalState == EditorTaskState::Failed) {
                    job->shared->phase = "Failed";
                }
            }
            --running_;
            if (queue_.empty() && running_ == 0) idleCondition_.notify_all();
        }
    }
}

} // namespace dve::editor
