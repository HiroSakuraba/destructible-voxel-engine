#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace dve {

// Small persistent worker pool for generation-keyed derived work. The authority thread remains
// the single writer. Only one coordinator may call parallel_for at a time.
class JobSystem {
public:
    explicit JobSystem(std::size_t workerCount = default_worker_count());
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
    [[nodiscard]] static std::size_t default_worker_count() noexcept;

    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& function);

private:
    std::vector<std::thread> workers_{};
    std::mutex mutex_{};
    std::condition_variable startCondition_{};
    std::condition_variable doneCondition_{};
    bool stopping_{};
    std::uint64_t generation_{};
    std::size_t completedWorkers_{};
    std::atomic<std::size_t> nextIndex_{0};
    std::size_t itemCount_{};
    std::function<void(std::size_t)> function_{};

    void worker_loop();
    void consume_batch();
};

} // namespace dve
