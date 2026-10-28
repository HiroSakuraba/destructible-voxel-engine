#pragma once
// PSO-2: bounded pipeline preparation before publishing streamed scene resources.
//
// This is intentionally RHI-owner-thread driven, not a background Vulkan call:
// future multithreaded compilation must externally synchronize the VkPipelineCache
// installed by PSO-1. Call pump() in a loading/scene-preparation phase, never in
// record_*_frame(). Transfer real pipeline handles only after all required jobs
// for a scene generation are ready.
#include "dve/rhi/device.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dve::rhi {
enum class PipelinePreparationState : std::uint8_t {
    Missing, Pending, Ready, Failed, Published
};
struct PipelinePreparationProgress {
    std::uint32_t attempted{}, compiled{}, failed{}, pending{};
    bool hitSoftBudget{};
    std::uint64_t elapsedNanoseconds{};
};
struct PipelinePreparationSummary {
    std::uint32_t pending{}, ready{}, failed{}, published{};
    bool allReady{}; // false if epoch is absent, pending or failed
};
class PipelinePreparationQueue final {
public:
    explicit PipelinePreparationQueue(IDevice& device) noexcept : device_(device) {}
    ~PipelinePreparationQueue() noexcept { clear(); }
    PipelinePreparationQueue(const PipelinePreparationQueue&) = delete;
    PipelinePreparationQueue& operator=(const PipelinePreparationQueue&) = delete;

    [[nodiscard]] bool stage_graphics(std::uint64_t epoch, std::string key,
        GraphicsPipelineDesc desc, std::string* error = nullptr);
    [[nodiscard]] bool stage_compute(std::uint64_t epoch, std::string key,
        ComputePipelineDesc desc, std::string* error = nullptr);

    // A single driver compilation is never preemptible. The soft budget is
    // checked before/after each job; maxPipelines caps progress deterministically.
    [[nodiscard]] PipelinePreparationProgress pump(std::uint32_t maxPipelines,
        std::chrono::microseconds softBudget = std::chrono::microseconds{2000});

    [[nodiscard]] PipelinePreparationState state(std::uint64_t epoch,
        std::string_view key) const noexcept;
    [[nodiscard]] PipelinePreparationSummary summary(std::uint64_t epoch) const noexcept;
    [[nodiscard]] bool ready_to_publish(std::uint64_t epoch) const noexcept {
        return summary(epoch).allReady;
    }

    // Transfer ownership; no new driver compilation occurs in these calls.
    // Missing, failed, stale and wrong-type requests return null handles.
    [[nodiscard]] GraphicsPipelineHandle take_graphics(
        std::uint64_t epoch, std::string_view key) noexcept;
    [[nodiscard]] ComputePipelineHandle take_compute(
        std::uint64_t epoch, std::string_view key) noexcept;
    [[nodiscard]] std::string_view failure_reason(
        std::uint64_t epoch, std::string_view key) const noexcept;

    // Cancelling or replacing a scene generation destroys unclaimed pipelines;
    // published handles are now owned by the consumer and never touched here.
    void discard_epoch(std::uint64_t epoch) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return jobs_.size(); }

private:
    struct Job {
        std::uint64_t epoch{};
        std::string key;
        PipelinePreparationState state{PipelinePreparationState::Pending};
        std::variant<GraphicsPipelineDesc, ComputePipelineDesc> desc;
        std::variant<std::monostate, GraphicsPipelineHandle, ComputePipelineHandle> prepared;
        std::string failure;
    };
    static constexpr std::size_t kMaximumJobs = 512U;
    IDevice& device_;
    std::vector<Job> jobs_;
    [[nodiscard]] Job* find(std::uint64_t epoch, std::string_view key) noexcept;
    [[nodiscard]] const Job* find(std::uint64_t epoch, std::string_view key) const noexcept;
    void retire(Job& job) noexcept;
};
} // namespace dve::rhi
