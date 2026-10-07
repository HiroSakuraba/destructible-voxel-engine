#pragma once

#include <array>
#include <cmath>
#include "dve/types.hpp"

namespace dve::editor {

// Two continuous-time first-order filters. Integrate their output analytically,
// rather than moving by the end-of-frame velocity: subdivision leaves travel unchanged.
class CameraNavigationFilter {
public:
    void reset() noexcept { acceleration_ = {}; smoothing_ = {}; }
    Float3 integrate(Float3 targetVelocity, double seconds, double acceleration, double smoothing) noexcept {
        if (!std::isfinite(seconds) || seconds <= 0.0) return {};
        const std::array<double, 3> target{targetVelocity.x, targetVelocity.y, targetVelocity.z};
        std::array<float, 3> displacement{};
        for (std::size_t i = 0; i < 3; ++i) {
            const double u = target[i], x = acceleration_[i], y = smoothing_[i];
            const double a = acceleration > 0.0 ? std::exp(-seconds / acceleration) : 0.0;
            const double b = smoothing > 0.0 ? std::exp(-seconds / smoothing) : 0.0;
            double integral{};
            if (acceleration <= 0.0) {
                acceleration_[i] = u;
                smoothing_[i] = u + (y - u) * b;
                integral = u * seconds + (y - u) * smoothing * (1.0 - b);
            } else if (smoothing <= 0.0) {
                acceleration_[i] = u + (x - u) * a;
                smoothing_[i] = acceleration_[i];
                integral = u * seconds + (x - u) * acceleration * (1.0 - a);
            } else if (std::fabs(acceleration - smoothing) <= 1.0e-6 * acceleration) {
                const double t = seconds / acceleration;
                acceleration_[i] = u + (x - u) * a;
                smoothing_[i] = u + (y - u) * a + (x - u) * t * a;
                integral = u * seconds + (y - u) * acceleration * (1.0 - a)
                    + (x - u) * acceleration * (1.0 - (1.0 + t) * a);
            } else {
                const double scale = acceleration / (acceleration - smoothing);
                acceleration_[i] = u + (x - u) * a;
                smoothing_[i] = u + (y - u) * b + (x - u) * scale * (a - b);
                integral = u * seconds + (y - u) * smoothing * (1.0 - b)
                    + (x - u) * scale * (acceleration * (1.0 - a) - smoothing * (1.0 - b));
            }
            displacement[i] = static_cast<float>(integral);
        }
        return {displacement[0], displacement[1], displacement[2]};
    }
private:
    std::array<double, 3> acceleration_{};
    std::array<double, 3> smoothing_{};
};
} // namespace dve::editor
