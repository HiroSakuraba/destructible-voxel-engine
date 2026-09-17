#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

#include "dve/fragment.hpp"

namespace {

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

} // namespace

int main() {
    using namespace dve;
    using Clock = std::chrono::steady_clock;

    VoxelObject fragment(12001);
    fragment.reserve_bricks(512);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 8; ++x) fragment.fill_brick({x, y, z}, 1);
        }
    }
    // Carve several shafts so the proxy is nontrivial while retaining a large
    // cross-brick slab that the merger can collapse.
    for (int z = 0; z < 16; ++z) {
        for (int y = 6; y < 10; ++y) {
            for (int x = 20; x < 24; ++x) fragment.set_voxel({x, y, z}, 0);
            for (int x = 44; x < 48; ++x) fragment.set_voxel({x, y, z}, 0);
        }
    }

    MaterialMassTable masses;
    masses.set_density_units(1, 24);
    constexpr int kIterations = 200;
    std::vector<double> samples;
    samples.reserve(kIterations);
    FragmentSolverPackage last;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const auto start = Clock::now();
        last = build_fragment_solver_package(
            fragment,
            make_rigid_transform({100.0F, 20.0F, -5.0F}, {}),
            masses,
            256);
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }

    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"voxels\": " << fragment.occupied_voxel_count() << ",\n"
              << "  \"mass_units\": " << last.mass.massUnits << ",\n"
              << "  \"unmerged_boxes\": " << last.unmergedBoxCount << ",\n"
              << "  \"merged_boxes\": " << last.boxes.size() << ",\n"
              << "  \"proxy_over_budget\": " << (last.proxyOverBudget ? "true" : "false") << ",\n"
              << "  \"package_ms\": {\"p50\": " << percentile(samples, 0.50)
              << ", \"p95\": " << percentile(samples, 0.95)
              << ", \"p99\": " << percentile(samples, 0.99) << "},\n"
              << "  \"center_of_mass\": [" << last.mass.centerOfMass.x << ", "
              << last.mass.centerOfMass.y << ", " << last.mass.centerOfMass.z << "]\n"
              << "}\n";
    return validate_solver_package(fragment, last) ? 0 : 1;
}
