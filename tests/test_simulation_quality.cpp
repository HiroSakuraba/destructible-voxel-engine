#include <cmath>
#include <cstdlib>
#include <iostream>

#include "dve/simulation_quality.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    dve::SimulationStepPolicy policy;
    policy.frameDeltaSeconds = 1.0F / 30.0F;
    policy.maximumSpeed = 10.0F;
    policy.characteristicLength = 0.1F;
    policy.maximumDisplacementFraction = 0.25F;
    policy.maximumSubsteps = 16U;
    const auto plan = dve::plan_simulation_step(policy);
    require(plan.validate(nullptr), "plan should validate");
    require(plan.substeps > 1U, "fast motion should produce substeps");
    require(plan.estimatedCfl <= 0.26F, "CFL should respect displacement fraction");

    policy.maximumSpeed = 1000.0F;
    policy.maximumSubsteps = 2U;
    const auto clamped = dve::plan_simulation_step(policy);
    require(clamped.clamped && clamped.substeps == 2U, "substep budget should clamp");
    std::cout << "simulation quality tests passed\n";
}
