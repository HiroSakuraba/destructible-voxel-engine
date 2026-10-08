#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dve {
// Shared scheduling policy, instantiated independently by every host/preview.
// Solver substeps are deliberately not part of this clock. Integer nanoseconds
// are the host boundary; rate-scaled phase keeps 60/120 Hz periods exact
// instead of rounding each tick.
struct TickRate {
    std::uint32_t numerator{60}, denominator{1};
};
[[nodiscard]] inline std::uint64_t tick_deadline_nanoseconds(std::uint64_t tick,
                                                             TickRate rate) noexcept {
    if (!rate.numerator || !rate.denominator)
        return std::numeric_limits<std::uint64_t>::max();
    const auto periodNumerator = 1000000000ULL * rate.denominator;
    const auto whole = tick / rate.numerator, remainder = tick % rate.numerator;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (whole && periodNumerator > maximum / whole)
        return maximum;
    const auto base = whole * periodNumerator;
    const auto tail = remainder * (periodNumerator / rate.numerator) +
                      (remainder * (periodNumerator % rate.numerator)) / rate.numerator;
    return tail > maximum - base ? maximum : base + tail;
}
struct RationalTimeScale {
    std::uint32_t numerator{1}, denominator{1};
    std::uint64_t remainder{};
    [[nodiscard]] std::uint64_t apply(std::uint64_t delta) noexcept {
        if (!denominator || remainder >= denominator)
            return 0;
        const auto whole = delta / denominator,
                   tail = (delta % denominator) * numerator + remainder;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        remainder = tail % denominator;
        if (whole && numerator > maximum / whole)
            return maximum;
        const auto base = whole * numerator, extra = tail / denominator;
        return extra > maximum - base ? maximum : base + extra;
    }
};
enum class BacklogPolicy : std::uint8_t { Drop, Retain };
struct SimulationClock {
    float fixedDeltaSeconds{1.0F / 60.0F};
    double maximumFrameSeconds{0.25};
    std::uint32_t maximumSteps{8};
    double accumulator{}; // legacy host checkpoint boundary, seconds
    BacklogPolicy backlogPolicy{BacklogPolicy::Drop};
    double droppedSeconds{};
    std::uint64_t tickCount{};
    [[nodiscard]] double period_seconds() const noexcept {
        if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds))
            return 0.0;
        const double rate = 1.0 / static_cast<double>(fixedDeltaSeconds);
        const double integral = std::round(rate);
        return integral >= 1.0 && std::abs(rate - integral) < 0.0001
                   ? 1.0 / integral
                   : static_cast<double>(fixedDeltaSeconds);
    }
    [[nodiscard]] std::uint32_t advance(double elapsedSeconds) noexcept {
        if (!(elapsedSeconds >= 0.0) || !std::isfinite(elapsedSeconds))
            return 0;
        // Carry conversion fractions: partitioning one second into 165 frames has
        // the same phase as a single one-second advance (before the explicit
        // backlog policy).
        const long double ns =
            static_cast<long double>(elapsedSeconds) * 1000000000.0L + fractionalNanoseconds_;
        if (ns <= 0.0L)
            return advance_nanoseconds(0);
        const auto bounded =
            std::min(ns, static_cast<long double>(std::numeric_limits<std::uint64_t>::max()));
        // Round the cumulative host time upward at the integer-nanosecond
        // boundary. Preserve the negative remainder, so this cannot add a
        // nanosecond every frame. A 1/30 s frame can release exactly two 60 Hz
        // steps instead of deferring the second for a 1/3 ns truncation.
        const auto whole = static_cast<std::uint64_t>(std::ceil(bounded));
        fractionalNanoseconds_ = bounded - static_cast<long double>(whole);
        return advance_nanoseconds(whole);
    }
    [[nodiscard]] std::uint32_t advance_nanoseconds(std::uint64_t elapsed) noexcept {
        const double period = period_seconds();
        if (!(period > 0.0) || !(maximumFrameSeconds >= 0.0) ||
            !std::isfinite(maximumFrameSeconds) || !maximumSteps || !(accumulator >= 0.0) ||
            !std::isfinite(accumulator))
            return 0;
        const double rate = 1.0 / period;
        const auto wholeRate = static_cast<std::uint64_t>(std::min(std::round(rate), 1000000.0));
        const bool integral = wholeRate && std::abs(rate - double(wholeRate)) < 0.0001;
        const std::uint64_t scale = integral ? wholeRate : 1U;
        const std::uint64_t threshold =
            integral ? 1000000000ULL
                     : static_cast<std::uint64_t>(std::max(1.0, std::round(period * 1.0e9)));
        if (accumulator != publishedAccumulator_ || scale != phaseScale_ ||
            threshold != threshold_) {
            const long double units = static_cast<long double>(accumulator) * 1000000000.0L * scale;
            phase_ = units >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
                         ? std::numeric_limits<std::uint64_t>::max()
                         : static_cast<std::uint64_t>(units);
        }
        phaseScale_ = scale;
        threshold_ = threshold;
        const auto maximum = static_cast<std::uint64_t>(
            std::min(std::ceil(static_cast<long double>(maximumFrameSeconds) * 1000000000.0L),
                     static_cast<long double>(std::numeric_limits<std::uint64_t>::max() / scale)));
        const auto accepted = std::min(elapsed, maximum);
        droppedSeconds += double(elapsed - accepted) / 1.0e9;
        const auto incoming = accepted * scale;
        phase_ = incoming > std::numeric_limits<std::uint64_t>::max() - phase_
                     ? std::numeric_limits<std::uint64_t>::max()
                     : phase_ + incoming;
        const auto steps =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(phase_ / threshold, maximumSteps));
        phase_ -= std::uint64_t(steps) * threshold;
        if (backlogPolicy == BacklogPolicy::Drop && phase_ >= threshold) {
            const auto retained = phase_ % threshold;
            droppedSeconds += double(phase_ - retained) / (1.0e9 * double(scale));
            phase_ = retained;
        }
        publishedAccumulator_ = accumulator = double(phase_) / (1.0e9 * double(scale));
        tickCount = steps > std::numeric_limits<std::uint64_t>::max() - tickCount
                        ? std::numeric_limits<std::uint64_t>::max()
                        : tickCount + steps;
        return steps;
    }
    [[nodiscard]] float alpha() const noexcept {
        const double period = period_seconds();
        return period > 0.0 ? static_cast<float>(std::clamp(accumulator / period, 0.0, 1.0)) : 0.0F;
    }

  private:
    std::uint64_t phase_{};
    std::uint64_t phaseScale_{};
    std::uint64_t threshold_{};
    double publishedAccumulator_{};
    long double fractionalNanoseconds_{};
};
// Convert an authored duration once. Never accumulate float elapsed world time.
[[nodiscard]] inline std::uint64_t simulation_nanoseconds(double seconds) noexcept {
    if (!(seconds > 0.0) || !std::isfinite(seconds))
        return 0;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const long double value = std::ceil(static_cast<long double>(seconds) * 1000000000.0L);
    return value >= static_cast<long double>(maximum) ? maximum : static_cast<std::uint64_t>(value);
}
[[nodiscard]] inline std::uint64_t saturating_time_add(std::uint64_t a, std::uint64_t b) noexcept {
    return b > std::numeric_limits<std::uint64_t>::max() - a
               ? std::numeric_limits<std::uint64_t>::max()
               : a + b;
}
} // namespace dve
