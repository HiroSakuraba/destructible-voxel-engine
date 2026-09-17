#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace dve {

struct FrameTelemetry {
    double cpuFrameMs{};
    double gpuFrameMs{};
    std::uint32_t dirtyBricks{};
    std::uint32_t structuralJobs{};
    std::uint64_t uploadBytes{};
    std::uint32_t awakeBodies{};
};

enum class PressureLevel : std::uint8_t { Normal, Elevated, Recovery, Emergency };

struct WorkBudget {
    PressureLevel pressure{PressureLevel::Normal};
    std::uint32_t nearDeadlineMicroseconds{900};
    std::uint32_t deferredMicroseconds{700};
    std::uint32_t patchBrickLimit{192};
    std::uint32_t structuralNodeLimit{4096};
    bool allowAsyncQualityUpgrades{true};
};

class WorkDebtGovernor {
public:
    explicit WorkDebtGovernor(std::size_t percentileWindow = 240);

    [[nodiscard]] WorkBudget update(const FrameTelemetry& telemetry);
    [[nodiscard]] double cpu_p99_ms() const;
    [[nodiscard]] double gpu_p99_ms() const;
    [[nodiscard]] PressureLevel pressure() const noexcept { return pressure_; }

private:
    std::size_t windowSize_{};
    std::deque<double> cpuFrames_{};
    std::deque<double> gpuFrames_{};
    double debtScore_{};
    std::uint32_t calmFrames_{};
    PressureLevel pressure_{PressureLevel::Normal};

    static double percentile99(const std::deque<double>& samples);
};

} // namespace dve
