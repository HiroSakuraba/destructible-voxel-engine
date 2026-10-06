// Desktop editor frame pacing: an input-interruptible wait for the rest of the frame
// budget, plus a floor between frame starts so input floods cannot drive uncapped
// rendering.
#include "dve/platform/frame_pacing.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using dve::platform::FramePacing;
using dve::platform::frame_budget_wait;
using dve::platform::frame_minimum_interval_sleep;
using std::chrono::milliseconds;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Simulates the desktop loop with a fake clock: each frame renders for renderSeconds,
// then waits; input (if any) arrives inputAfterSeconds into the wait. Returns the
// mean frame interval.
double simulate(const FramePacing& pacing, double renderSeconds, double inputAfterSeconds, int frames) {
    double clock = 100.0;
    const double first = clock;
    for (int i = 0; i < frames; ++i) {
        const double frameStart = clock;
        clock += renderSeconds;
        const milliseconds wait = frame_budget_wait(pacing, frameStart, clock);
        if (wait.count() > 0) {
            const double waitSeconds = static_cast<double>(wait.count()) / 1000.0;
            clock += inputAfterSeconds >= 0.0 && inputAfterSeconds < waitSeconds ? inputAfterSeconds : waitSeconds;
        }
        const milliseconds floorSleep = frame_minimum_interval_sleep(pacing, frameStart, clock);
        clock += static_cast<double>(floorSleep.count()) / 1000.0;
    }
    return (clock - first) / frames;
}

void test_waits() {
    const FramePacing pacing{};  // 8 ms budget, 4 ms floor
    require(frame_budget_wait(pacing, 10.0, 10.002) == milliseconds{5}, "a 2 ms frame should wait out the budget");
    require(frame_budget_wait(pacing, 10.0, 10.0075) == milliseconds{0}, "under 1 ms left should not wait");
    require(frame_budget_wait(pacing, 10.0, 10.012) == milliseconds{0}, "an over-budget frame should not wait");
    require(frame_minimum_interval_sleep(pacing, 10.0, 10.0025) == milliseconds{1},
            "an early input wake should sleep up to the floor");
    require(frame_minimum_interval_sleep(pacing, 10.0, 10.008) == milliseconds{0},
            "a full-budget timeout should not add a floor sleep");
    require(frame_minimum_interval_sleep(pacing, 10.0, 10.006) == milliseconds{0},
            "a frame longer than the floor should not sleep");
    std::printf("frame pacing waits: OK\n");
}

void test_simulated_rates() {
    const FramePacing pacing{};
    // Idle: no input, cheap frames -> one frame per budget (about 125 fps).
    const double idle = simulate(pacing, 0.0005, -1.0, 400);
    require(idle > 0.0069 && idle < 0.0081, "idle frames should be paced by the budget");
    // Mouse-motion flood: an event 0.1 ms into every wait. Without the floor this
    // would run at about 1 frame per 0.6 ms; with it, frames stay at least ~3-4 ms apart.
    const double flood = simulate(pacing, 0.0005, 0.0001, 400);
    require(flood >= 0.003 && flood <= 0.0041, "input floods should be capped by the floor");
    FramePacing unfloored = pacing;
    unfloored.minimumIntervalSeconds = 0.0;
    require(simulate(unfloored, 0.0005, 0.0001, 400) < 0.001, "the simulation should show the uncapped rate");
    // Heavy frames: no waiting at all; the interval is the frame time.
    const double heavy = simulate(pacing, 0.012, 0.0001, 100);
    require(heavy > 0.0119 && heavy < 0.0121, "over-budget frames should never sleep");
    std::printf("frame pacing simulation: OK (idle %.2f ms, flood %.2f ms, heavy %.2f ms per frame)\n",
                idle * 1000.0, flood * 1000.0, heavy * 1000.0);
}

} // namespace

int main() {
    try {
        test_waits();
        test_simulated_rates();
        std::printf("frame pacing tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
