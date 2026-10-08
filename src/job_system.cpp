#include "dve/job_system.hpp"

#include <algorithm>

namespace dve {
namespace {
struct ActivePool {
    const JobSystem* pool;
    ActivePool* parent;
    static thread_local ActivePool* current;
    explicit ActivePool(const JobSystem* value) : pool(value), parent(current) { current=this; }
    ~ActivePool() { current=parent; }
    static bool contains(const JobSystem* value) {
        for (auto* p=current;p;p=p->parent) if(p->pool==value) return true;
        return false;
    }
};
thread_local ActivePool* ActivePool::current{};
}


std::size_t JobSystem::default_worker_count() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 1U) return 0;
    return static_cast<std::size_t>(hardware - 1U);
}

JobSystem::JobSystem(std::size_t workerCount) {
    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) workers_.emplace_back([this] { worker_loop(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        ++generation_;
    }
    startCondition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void JobSystem::consume_batch() {
    ActivePool active(this);
    for (;;) {
        const std::size_t index = nextIndex_.fetch_add(1, std::memory_order_relaxed);
        if (index >= itemCount_) return;
        try { function_(index); }
        catch (...) {
            std::lock_guard exceptionLock(exceptionMutex_);
            if (!exception_) exception_=std::current_exception();
        }
    }
}

void JobSystem::parallel_for(std::size_t count, const std::function<void(std::size_t)>& function) {
    if (count == 0) return;
    if (ActivePool::contains(this)) {
        for (std::size_t i=0;i<count;++i) function(i);
        return;
    }
    std::lock_guard coordinatorLock(coordinatorMutex_);
    ActivePool active(this);
    if (workers_.empty() || count == 1) {
        for (std::size_t i = 0; i < count; ++i) function(i);
        return;
    }

    {
        std::lock_guard lock(mutex_);
        exception_ = {};
        itemCount_ = count;
        function_ = function;
        nextIndex_.store(0, std::memory_order_relaxed);
        completedWorkers_ = 0;
        ++generation_;
    }
    startCondition_.notify_all();

    // The authority/coordinator thread participates instead of blocking immediately.
    consume_batch();

    std::unique_lock lock(mutex_);
    doneCondition_.wait(lock, [this] { return completedWorkers_ == workers_.size(); });
    function_ = {};
    itemCount_ = 0;
    if (exception_) std::rethrow_exception(exception_);
}

void JobSystem::worker_loop() {
    std::uint64_t observedGeneration = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            startCondition_.wait(lock, [this, &observedGeneration] {
                return stopping_ || generation_ != observedGeneration;
            });
            if (stopping_) return;
            observedGeneration = generation_;
        }

        consume_batch();

        {
            std::lock_guard lock(mutex_);
            ++completedWorkers_;
            if (completedWorkers_ == workers_.size()) doneCondition_.notify_one();
        }
    }
}

} // namespace dve
