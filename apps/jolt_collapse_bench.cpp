// Jolt collapse/debris benchmark for the 60 Hz physics contract.
// This executable is intentionally separate from the correctness smoke test: its numbers are
// hardware-specific and should be recorded per target machine rather than used as CI constants.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "dve/physics_jolt_backend.hpp"
#include "dve/transform.hpp"

namespace {

std::size_t percentile_index(std::size_t count, double fraction) {
    if (count == 0U) return 0U;
    return std::min(count - 1U, static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(count))) - 1U);
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    const std::size_t index = percentile_index(values.size(), fraction);
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

std::uint32_t parse_u32(const char* text, std::uint32_t fallback) {
    if (text == nullptr) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 0xFFFFFFFFUL) return fallback;
    return static_cast<std::uint32_t>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    using namespace dve;

    std::uint32_t bodyCount = 256U;
    std::uint32_t workerThreads = 4U;
    std::uint32_t steps = 600U;
    bool debrisNoSelf = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--bodies" && i + 1 < argc) bodyCount = parse_u32(argv[++i], bodyCount);
        else if (argument == "--workers" && i + 1 < argc) workerThreads = parse_u32(argv[++i], workerThreads);
        else if (argument == "--steps" && i + 1 < argc) steps = parse_u32(argv[++i], steps);
        else if (argument == "--debris-no-self") debrisNoSelf = true;
    }
    bodyCount = std::clamp(bodyCount, 1U, 4096U);
    steps = std::clamp(steps, 60U, 10000U);

    JoltWorldConfig config;
    config.workerThreads = workerThreads;
    config.maxBodies = std::max(32768U, bodyCount + 1024U);
    config.maxBodyPairs = std::max(131072U, bodyCount * 512U);
    config.maxContactConstraints = std::max(65536U, bodyCount * 128U);
    JoltRigidBodyWorld world(config);
    world.set_gravity({0.0F, -9.81F, 0.0F});

    const RigidBodyHandle floor = world.create_static_box(
        make_rigid_transform({0.0F, -1.0F, 0.0F}, {}), {100.0F, 1.0F, 100.0F});
    if (floor == kInvalidRigidBodyHandle) {
        std::fprintf(stderr, "failed to create floor\n");
        return EXIT_FAILURE;
    }

    constexpr float half = 0.25F;
    constexpr double mass = 1.0;
    constexpr double inertia = mass / 6.0 * (0.5 * 0.5);
    const std::uint32_t width = static_cast<std::uint32_t>(std::ceil(std::cbrt(static_cast<double>(bodyCount))));
    std::vector<RigidBodyCreateDesc> descriptors;
    descriptors.reserve(bodyCount);
    for (std::uint32_t i = 0; i < bodyCount; ++i) {
        const std::uint32_t x = i % width;
        const std::uint32_t z = (i / width) % width;
        const std::uint32_t y = i / (width * width);
        RigidBodyCreateDesc desc;
        desc.transform = make_rigid_transform(
            {static_cast<float>(x) * 0.51F - static_cast<float>(width) * 0.255F,
             0.25F + static_cast<float>(y) * 0.51F + 0.5F,
             static_cast<float>(z) * 0.51F - static_cast<float>(width) * 0.255F},
            {});
        desc.massKilograms = mass;
        desc.inertiaKilogramMetersSquared = {inertia, inertia, inertia, 0.0, 0.0, 0.0};
        desc.boxes = {SolverBox{{}, {half, half, half}}};
        desc.collisionClass = debrisNoSelf
            ? RigidBodyCollisionClass::DebrisNoSelf
            : RigidBodyCollisionClass::Full;
        descriptors.push_back(std::move(desc));
    }

    const std::vector<RigidBodyHandle> handles = world.create_bodies(descriptors);
    const std::size_t created = static_cast<std::size_t>(std::count_if(
        handles.begin(), handles.end(), [](RigidBodyHandle handle) { return handle != kInvalidRigidBodyHandle; }));
    if (created != descriptors.size()) {
        std::fprintf(stderr, "created only %zu of %zu bodies\n", created, descriptors.size());
        return EXIT_FAILURE;
    }

    std::vector<double> stepMilliseconds;
    stepMilliseconds.reserve(steps);
    constexpr float fixedDelta = 1.0F / 60.0F;
    for (std::uint32_t step = 0; step < steps; ++step) {
        const auto start = std::chrono::steady_clock::now();
        world.step(fixedDelta);
        const auto finish = std::chrono::steady_clock::now();
        stepMilliseconds.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
        if (!world.last_update_succeeded()) break;
    }

    const JoltPhysicsTelemetry telemetry = world.telemetry();
    const double maximum = stepMilliseconds.empty()
        ? 0.0
        : *std::max_element(stepMilliseconds.begin(), stepMilliseconds.end());
    std::printf(
        "{\n"
        "  \"requested_bodies\": %u,\n"
        "  \"created_bodies\": %zu,\n"
        "  \"worker_threads\": %u,\n"
        "  \"debris_self_collision\": %s,\n"
        "  \"steps_recorded\": %zu,\n"
        "  \"step_ms_p50\": %.6f,\n"
        "  \"step_ms_p95\": %.6f,\n"
        "  \"step_ms_p99\": %.6f,\n"
        "  \"step_ms_max\": %.6f,\n"
        "  \"awake_dynamic_bodies_end\": %zu,\n"
        "  \"sleeping_dynamic_bodies_end\": %zu,\n"
        "  \"update_error_bits\": %u,\n"
        "  \"failed_update_calls\": %llu\n"
        "}\n",
        bodyCount,
        created,
        telemetry.configuredWorkerThreads,
        debrisNoSelf ? "false" : "true",
        stepMilliseconds.size(),
        percentile(stepMilliseconds, 0.50),
        percentile(stepMilliseconds, 0.95),
        percentile(stepMilliseconds, 0.99),
        maximum,
        telemetry.bodyCounts.awakeDynamicBodies,
        telemetry.bodyCounts.sleepingDynamicBodies,
        telemetry.lastUpdateErrorBits,
        static_cast<unsigned long long>(telemetry.failedUpdateCalls));

    return telemetry.failedUpdateCalls == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
