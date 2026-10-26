#include "dve/region_streaming_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dve {

RegionStreamingRuntime::RegionStreamingRuntime(const RegionCatalog& catalog, RuntimeSceneWorld& world,
    RuntimeSceneStagingExecutor& executor, RuntimeSceneHandle sceneHandle,
    RegionStreamingRuntimeConfig config)
    : catalog_(catalog), world_(world), executor_(executor), sceneHandle_(sceneHandle), config_(std::move(config)),
      planner_(config_.planner) {
    if (!planner_.valid() || config_.maximumInFlight == 0 || config_.maximumPublicationsPerTick == 0)
        throw std::invalid_argument("invalid region streaming runtime configuration");
}

RegionStreamingRuntime::~RegionStreamingRuntime() {
    for (auto& [_, job] : pending_) job.cancellation.request_cancel();
}

void RegionStreamingRuntime::set_pinned(std::uint64_t id, bool pinned) {
    if (pinned) pins_.insert(id);
    else pins_.erase(id);
}
void RegionStreamingRuntime::allow_static_unload(std::uint64_t id, bool allowed) {
    if (allowed) unloadAllowed_.insert(id);
    else unloadAllowed_.erase(id);
}

RegionStreamingTickStats RegionStreamingRuntime::tick(RegionKey camera, std::uint64_t tick) {
    RegionStreamingTickStats stats{};
    const RuntimeScene* scene = world_.scene(sceneHandle_);
    if (!scene) {
        stats.sceneMissing = true;
        for (auto& [_, job] : pending_) job.cancellation.request_cancel();
        stats.pending = pending_.size();
        return stats;
    }

    std::vector<RegionStreamingObject> objects;
    objects.reserve(scene->objects().size());
    for (const auto& object : scene->objects()) {
        const auto id = object.metadata().id;
        if (object.loaded()) loadedAt_.try_emplace(id, tick);
        else loadedAt_.erase(id);
        objects.push_back({id, object.loaded(), pins_.contains(id),
            config_.enableStaticUnloads && object.metadata().anchored && unloadAllowed_.contains(id),
            loadedAt_.contains(id) ? loadedAt_.at(id) : tick, pending_.contains(id)});
    }
    const auto plan = planner_.plan(catalog_, camera, objects, tick);
    if (!plan) {
        stats.planRejected = true;
        // Do not unload on budget failure. Cancel pending speculative work only
        // when a successful subsequent query proves it is out of range.
        stats.pending = pending_.size();
        return stats;
    }
    const auto near = [&](std::uint64_t id) {
        return std::binary_search(plan->nearIds.begin(), plan->nearIds.end(), id);
    };

    // First cancel obsolete work, then consume ready futures. Never block a frame
    // on an in-progress background decode/derived-data task.
    std::vector<std::uint64_t> ready;
    for (auto& [id, job] : pending_) {
        if (!near(id) && !job.cancelRequested) {
            job.cancellation.request_cancel();
            job.cancelRequested = true;
            ++stats.cancelled;
        }
        if (job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            ready.push_back(id);
    }
    std::sort(ready.begin(), ready.end());
    std::size_t publishedThisTick = 0;
    for (const auto id : ready) {
        if (near(id) && publishedThisTick >= config_.maximumPublicationsPerTick) continue;
        auto iter = pending_.find(id);
        if (iter == pending_.end()) continue;
        try {
            auto result = iter->second.future.get();
            ++stats.completed;
            const auto* current = world_.scene(sceneHandle_);
            const auto* object = current ? current->find_object(id) : nullptr;
            if (!near(id) || !object || object->loaded()) {
                ++stats.discarded;
            } else if (result.error) {
                ++stats.errors;
                failedAt_[id] = tick;
            } else {
                RuntimeSceneError error;
                if (world_.publish_staged_deferred_object(std::move(result.staging), &error)) {
                    ++stats.published;
                    ++publishedThisTick;
                    loadedAt_[id] = tick;
                    failedAt_.erase(id);
                } else {
                    ++stats.errors;
                    failedAt_[id] = tick;
                }
            }
        } catch (...) {
            ++stats.errors;
            failedAt_[id] = tick;
        }
        pending_.erase(iter);
    }

    // No automatic eviction of dynamic bodies or edited static objects. Callers
    // must explicitly whitelist an anchored object, with R4 state safeguards.
    for (const auto id : plan->unloadIds) {
        const auto* current = world_.scene(sceneHandle_);
        const auto* object = current ? current->find_object(id) : nullptr;
        if (!object || !object->loaded() || pins_.contains(id) || !object->metadata().anchored ||
            !config_.enableStaticUnloads || !unloadAllowed_.contains(id)) continue;
        RuntimeSceneError error;
        if (world_.unload_object(sceneHandle_, id, &error)) {
            ++stats.unloaded;
            loadedAt_.erase(id);
        } else ++stats.errors;
    }

    for (const auto id : plan->loadIds) {
        if (pending_.size() >= config_.maximumInFlight) break;
        if (pending_.contains(id)) continue;
        const auto* current = world_.scene(sceneHandle_);
        const auto* object = current ? current->find_object(id) : nullptr;
        if (!object || object->loaded()) continue;
        if (const auto failed = failedAt_.find(id); failed != failedAt_.end() &&
            (tick < failed->second || tick - failed->second < config_.retryDelayTicks)) continue;
        try {
            RuntimeSceneStageOptions stage = config_.stage;
            stage.cancellation = RuntimeSceneCancellationToken{};
            auto future = executor_.submit_deferred_object(world_, sceneHandle_, id,
                stage, config_.submission);
            pending_.emplace(id, Pending{std::move(future), stage.cancellation, false});
            ++stats.submitted;
        } catch (...) {
            ++stats.errors;
            failedAt_[id] = tick;
        }
    }
    stats.pending = pending_.size();
    return stats;
}
} // namespace dve

