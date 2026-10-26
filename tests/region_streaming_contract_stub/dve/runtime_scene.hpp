#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dve {
using RuntimeSceneHandle = std::uint32_t;
struct RuntimeSceneError {
    bool failed{};
    explicit operator bool() const noexcept { return failed; }
};
struct RuntimeSceneCancellationToken {
    std::shared_ptr<std::atomic_bool> state = std::make_shared<std::atomic_bool>(false);
    void request_cancel() const { state->store(true); }
    bool cancellation_requested() const { return state->load(); }
};
struct RuntimeSceneStageOptions {
    RuntimeSceneCancellationToken cancellation{};
};
struct RuntimeSceneSubmissionOptions {};
struct RuntimeDeferredObjectStaging {
    RuntimeSceneHandle scene{};
    std::uint64_t id{};
};
struct RuntimeDeferredObjectStageResult {
    RuntimeDeferredObjectStaging staging{};
    RuntimeSceneError error{};
};
struct RuntimeSceneObjectMetadata {
    std::uint64_t id{};
    bool anchored{};
};
class RuntimeSceneObject {
public:
    RuntimeSceneObjectMetadata meta{};
    bool resident{};
    [[nodiscard]] const RuntimeSceneObjectMetadata& metadata() const noexcept { return meta; }
    [[nodiscard]] bool loaded() const noexcept { return resident; }
};
class RuntimeScene {
public:
    std::vector<RuntimeSceneObject> list;
    [[nodiscard]] std::span<const RuntimeSceneObject> objects() const { return list; }
    [[nodiscard]] const RuntimeSceneObject* find_object(std::uint64_t id) const {
        for (const auto& obj: list) if (obj.meta.id == id) return &obj;
        return nullptr;
    }
};
class RuntimeSceneWorld {
public:
    RuntimeScene store;
    bool present = true;
    std::size_t published{};
    std::size_t unloaded{};
    [[nodiscard]] const RuntimeScene* scene(RuntimeSceneHandle) const { return present ? &store : nullptr; }
    [[nodiscard]] bool publish_staged_deferred_object(RuntimeDeferredObjectStaging&& staging, RuntimeSceneError*) {
        for (auto& obj: store.list) if(obj.meta.id==staging.id) { obj.resident=true; ++published; return true; }
        return false;
    }
    [[nodiscard]] bool unload_object(RuntimeSceneHandle, std::uint64_t id, RuntimeSceneError*) {
        for(auto& obj:store.list) if(obj.meta.id==id) { obj.resident=false; ++unloaded; return true; }
        return false;
    }
};
class RuntimeSceneStagingExecutor {
public:
    std::map<std::uint64_t, std::promise<RuntimeDeferredObjectStageResult>> queued;
    std::map<std::uint64_t, RuntimeSceneCancellationToken> tokens;
    std::uint64_t submissions{};
    std::future<RuntimeDeferredObjectStageResult> submit_deferred_object(
        const RuntimeSceneWorld&, RuntimeSceneHandle, std::uint64_t id,
        RuntimeSceneStageOptions opts, RuntimeSceneSubmissionOptions) {
        ++submissions;
        tokens[id] = opts.cancellation;
        auto& p = queued[id];
        return p.get_future();
    }
    void complete(std::uint64_t id) {
        auto p=std::move(queued.at(id)); queued.erase(id);
        p.set_value({{1,id},{false}});
    }
};
}

