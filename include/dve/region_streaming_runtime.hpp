#pragma once
// R2 bridge: use DVE's existing authority-thread publication and bounded staging executor.
// Load-only by default. Persistent modified voxel state is NOT yet transferred on unload.
#include "dve/region_streaming.hpp"
#include "dve/runtime_scene.hpp"
#include <future>
#include <functional>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dve {

struct RegionStreamingRuntimeConfig {
    RegionStreamingConfig planner{};
    std::size_t maximumInFlight{4};
    std::size_t maximumPublicationsPerTick{2};
    std::uint64_t retryDelayTicks{60};
    bool enableStaticUnloads{false}; // off until R4 persistence/rollback integration
    RuntimeSceneStageOptions stage{};
    RuntimeSceneSubmissionOptions submission{};
    // Optional PSO-2 publication gate, called on the authority thread.
    // False defers completed staging without repeating the worker job.
    // Callback and any captured pipeline queue must outlive this streamer.
    std::function<bool(std::uint64_t objectId)> pipelinesReadyForPublication;
};

struct RegionStreamingTickStats {
    bool planRejected{};
    bool sceneMissing{};
    bool wrongThread{};
    std::size_t submitted{};
    std::size_t completed{};
    std::size_t published{};
    std::size_t cancelled{};
    std::size_t discarded{};
    std::size_t unloaded{};
    std::size_t errors{};
    std::size_t pending{};
    std::size_t waitingForPipelines{};
};

class RegionStreamingRuntime {
public:
    // `world` and `executor` must outlive this object and its outstanding jobs.
    // All methods are authority-thread-only, including destruction; executor must
    // remain alive while work drains. No mutable raw pointers are passed to workers.
    RegionStreamingRuntime(const RegionCatalog& catalog, RuntimeSceneWorld& world, RuntimeSceneStagingExecutor& executor,
                           RuntimeSceneHandle sceneHandle, RegionStreamingRuntimeConfig config = {});
    ~RegionStreamingRuntime();
    RegionStreamingRuntime(const RegionStreamingRuntime&) = delete;
    RegionStreamingRuntime& operator=(const RegionStreamingRuntime&) = delete;

    void set_pinned(std::uint64_t objectId, bool pinned);
    void allow_static_unload(std::uint64_t objectId, bool allowed);
    [[nodiscard]] RegionStreamingTickStats tick(RegionKey camera, std::uint64_t tick);
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }
private:
    struct Pending {
        std::future<RuntimeDeferredObjectStageResult> future;
        std::optional<RuntimeDeferredObjectStageResult> readyResult;
        RuntimeSceneCancellationToken cancellation{};
        bool cancelRequested{};
    };
    [[nodiscard]] bool in_retry_backoff(std::uint64_t id, std::uint64_t tick) const;
    const RegionCatalog& catalog_;
    RuntimeSceneWorld& world_;
    RuntimeSceneStagingExecutor& executor_;
    RuntimeSceneHandle sceneHandle_{};
    std::thread::id authorityThread_{};
    RegionStreamingRuntimeConfig config_{};
    RegionStreamingPlanner planner_;
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::unordered_map<std::uint64_t, std::uint64_t> loadedAt_;
    std::unordered_map<std::uint64_t, std::uint64_t> failedAt_;
    std::unordered_set<std::uint64_t> pins_;
    std::unordered_set<std::uint64_t> unloadAllowed_;
};

} // namespace dve

