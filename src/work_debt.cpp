#include "dve/work_debt.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dve {

WorkDebtGovernor::WorkDebtGovernor(std::size_t percentileWindow)
    : windowSize_(std::max<std::size_t>(32, percentileWindow)) {}

double WorkDebtGovernor::percentile99(const std::deque<double>& samples) {
    if (samples.empty()) return 0.0;
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = static_cast<std::size_t>(
        std::floor(0.99 * static_cast<double>(sorted.size() - 1)));
    return sorted[index];
}

double WorkDebtGovernor::cpu_p99_ms() const { return percentile99(cpuFrames_); }
double WorkDebtGovernor::gpu_p99_ms() const { return percentile99(gpuFrames_); }

WorkBudget WorkDebtGovernor::update(const FrameTelemetry& telemetry) {
    cpuFrames_.push_back(telemetry.cpuFrameMs);
    gpuFrames_.push_back(telemetry.gpuFrameMs);
    if (cpuFrames_.size() > windowSize_) cpuFrames_.pop_front();
    if (gpuFrames_.size() > windowSize_) gpuFrames_.pop_front();

    const double framePressure = std::max(telemetry.cpuFrameMs, telemetry.gpuFrameMs) / 8.333;
    const double dirtyPressure = static_cast<double>(telemetry.dirtyBricks) / 256.0;
    const double structurePressure = static_cast<double>(telemetry.structuralJobs) / 128.0;
    const double uploadPressure = static_cast<double>(telemetry.uploadBytes) / (8.0 * 1024.0 * 1024.0);
    const double bodyPressure = static_cast<double>(telemetry.awakeBodies) / 128.0;
    const double instantaneous = std::max({framePressure, dirtyPressure, structurePressure, uploadPressure, bodyPressure});

    // Smooth queue debt. A burst raises pressure quickly; recovery is deliberately slower.
    const double riseAlpha = 0.30;
    const double fallAlpha = 0.04;
    const double alpha = instantaneous > debtScore_ ? riseAlpha : fallAlpha;
    debtScore_ += alpha * (instantaneous - debtScore_);

    const double p99 = std::max(cpu_p99_ms(), gpu_p99_ms());
    PressureLevel desired = PressureLevel::Normal;
    if (p99 > 12.5 || debtScore_ > 2.0) desired = PressureLevel::Emergency;
    else if (p99 > 8.333 || debtScore_ > 1.15) desired = PressureLevel::Recovery;
    else if (p99 > 7.5 || debtScore_ > 0.80) desired = PressureLevel::Elevated;

    if (static_cast<int>(desired) > static_cast<int>(pressure_)) {
        pressure_ = desired;
        calmFrames_ = 0;
    } else if (desired == PressureLevel::Normal) {
        ++calmFrames_;
        if (calmFrames_ >= 120 && pressure_ != PressureLevel::Normal) {
            pressure_ = static_cast<PressureLevel>(static_cast<int>(pressure_) - 1);
            calmFrames_ = 0;
        }
    } else {
        calmFrames_ = 0;
    }

    switch (pressure_) {
        case PressureLevel::Normal:
            return {pressure_, 900, 700, 192, 4096, true};
        case PressureLevel::Elevated:
            return {pressure_, 850, 400, 144, 3072, true};
        case PressureLevel::Recovery:
            return {pressure_, 800, 150, 96, 2048, false};
        case PressureLevel::Emergency:
            return {pressure_, 700, 0, 48, 1024, false};
    }
    return {};
}

} // namespace dve
