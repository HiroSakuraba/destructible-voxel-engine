#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "dve/query.hpp"

namespace {
using Clock = std::chrono::steady_clock;

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = p * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(position);
    const auto high = std::min(low + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(low);
    return values[low] * (1.0 - fraction) + values[high] * fraction;
}
}

int main() {
    using namespace dve;
    VoxelObject world(2001);
    world.reserve_bricks(512);
    for (int y = -48; y < 56; ++y) {
        for (int x = -48; x < 56; ++x) world.set_voxel({x, y, 0}, 1);
    }
    for (int z = 1; z < 8; ++z) {
        for (int y = -8; y <= 8; ++y) {
            world.set_voxel({12, y, z}, 2);
        }
    }

    const RigidTransform transform{};

    std::vector<double> sweepTimes;
    std::vector<double> resolutionTimes;
    sweepTimes.reserve(12000);
    resolutionTimes.reserve(12000);
    std::uint32_t randomState = 0xD00DFEEDU;
    auto random = [&]() {
        randomState = randomState * 1664525U + 1013904223U;
        return randomState;
    };
    std::size_t hitCount = 0;
    std::size_t groundedCount = 0;
    for (int frame = 0; frame < 12000; ++frame) {
        const Float3 base{
            static_cast<float>(static_cast<int>(random() % 1800U) - 900) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 1800U) - 900) / 100.0F,
            1.45F + static_cast<float>(random() % 20U) / 100.0F,
        };
        const Capsule capsule{base, add(base, {0.0F, 0.0F, 1.0F}), 0.45F};
        const Float3 displacement{
            static_cast<float>(static_cast<int>(random() % 100U) - 50) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 100U) - 50) / 100.0F,
            static_cast<float>(static_cast<int>(random() % 30U) - 15) / 100.0F,
        };

        const auto sweepStart = Clock::now();
        const auto hit = sweep_capsule_conservative(world, transform, capsule, displacement);
        const auto sweepEnd = Clock::now();
        sweepTimes.push_back(std::chrono::duration<double, std::milli>(sweepEnd - sweepStart).count());
        hitCount += hit.has_value() ? 1U : 0U;

        PlayerCollisionState player{capsule, {}, true};
        const auto resolveStart = Clock::now();
        const PlayerEditResolution resolution = resolve_player_after_voxel_edit(world, transform, player);
        const auto resolveEnd = Clock::now();
        resolutionTimes.push_back(std::chrono::duration<double, std::milli>(resolveEnd - resolveStart).count());
        groundedCount += resolution.grounded ? 1U : 0U;
    }

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\n";
    std::cout << "  \"queries\": " << sweepTimes.size() << ",\n";
    std::cout << "  \"sweep_ms\": {\"p50\": " << percentile(sweepTimes, 0.50)
              << ", \"p95\": " << percentile(sweepTimes, 0.95)
              << ", \"p99\": " << percentile(sweepTimes, 0.99) << "},\n";
    std::cout << "  \"post_edit_resolution_ms\": {\"p50\": " << percentile(resolutionTimes, 0.50)
              << ", \"p95\": " << percentile(resolutionTimes, 0.95)
              << ", \"p99\": " << percentile(resolutionTimes, 0.99) << "},\n";
    std::cout << "  \"hits\": " << hitCount << ",\n";
    std::cout << "  \"grounded\": " << groundedCount << "\n";
    std::cout << "}\n";
    return 0;
}
