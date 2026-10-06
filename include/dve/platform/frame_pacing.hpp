#pragma once

#include <chrono>

namespace dve::platform {

// Desktop editor frame pacing.
//
// Each frame has a total budget covering update, render and present. Whatever is
// left of it is spent in an input-interruptible wait, so a click is handled as soon
// as it arrives instead of after a fixed post-frame sleep. An input wake starts the
// next frame early, but never sooner than minimumIntervalSeconds after the previous
// frame started: without that floor, a stream of mouse-motion events (500-1000 Hz
// on many mice) drives rendering as fast as events arrive, because the editor's
// SDL renderer does not wait for vsync.
//
// Waits are whole milliseconds and are skipped below 1 ms, so the effective floor
// is between minimumIntervalSeconds - 1 ms and minimumIntervalSeconds.
struct FramePacing {
    double budgetSeconds{0.008};
    double minimumIntervalSeconds{0.004};
};

namespace detail {
[[nodiscard]] inline std::chrono::milliseconds whole_milliseconds(double seconds) noexcept {
    const double count = seconds * 1000.0;
    if (!(count >= 1.0)) return std::chrono::milliseconds{0};
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(count)};
}
} // namespace detail

// Interruptible wait to perform after rendering a frame that started at frameStart
// (seconds on the host's monotonic clock). Zero when the budget is already spent.
[[nodiscard]] inline std::chrono::milliseconds frame_budget_wait(
    const FramePacing& pacing, double frameStart, double now) noexcept {
    return detail::whole_milliseconds(frameStart + pacing.budgetSeconds - now);
}

// Uninterruptible sleep to perform after that wait returns, so the next frame does
// not start sooner than the minimum interval. Zero after a timeout (the budget is
// longer than the interval) or when the frame itself took long enough.
[[nodiscard]] inline std::chrono::milliseconds frame_minimum_interval_sleep(
    const FramePacing& pacing, double frameStart, double now) noexcept {
    return detail::whole_milliseconds(frameStart + pacing.minimumIntervalSeconds - now);
}

} // namespace dve::platform
