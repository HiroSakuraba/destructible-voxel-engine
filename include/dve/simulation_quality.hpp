#pragma once

#include <cstdint>
#include <string>

namespace dve {

struct SimulationStepPolicy {
    float frameDeltaSeconds{1.0F / 60.0F};
    float maximumSpeed{};
    float characteristicLength{0.1F};
    float maximumDisplacementFraction{0.4F};
    float maximumSubstepSeconds{1.0F / 60.0F};
    std::uint32_t maximumSubsteps{8U};
};

struct SimulationStepPlan {
    bool enabled{};
    bool clamped{};
    std::uint32_t substeps{};
    float substepSeconds{};
    float estimatedCfl{};
    float requestedSubstepSeconds{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] SimulationStepPlan plan_simulation_step(const SimulationStepPolicy& policy);

} // namespace dve
