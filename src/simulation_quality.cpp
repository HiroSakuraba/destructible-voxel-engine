#include "dve/simulation_quality.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dve {
namespace {
void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}
}

bool SimulationStepPlan::validate(std::string* error) const {
    if (!enabled) return true;
    if (substeps == 0U || !(substepSeconds > 0.0F) ||
        !std::isfinite(substepSeconds) || !std::isfinite(estimatedCfl) ||
        !std::isfinite(requestedSubstepSeconds)) {
        set_error(error, "enabled simulation step plan is invalid");
        return false;
    }
    return true;
}

SimulationStepPlan plan_simulation_step(const SimulationStepPolicy& policy) {
    SimulationStepPlan plan;
    if (!(policy.frameDeltaSeconds > 0.0F) || !std::isfinite(policy.frameDeltaSeconds) ||
        !(policy.characteristicLength > 0.0F) || !std::isfinite(policy.characteristicLength) ||
        !(policy.maximumDisplacementFraction > 0.0F) ||
        !std::isfinite(policy.maximumDisplacementFraction) ||
        !(policy.maximumSubstepSeconds > 0.0F) ||
        !std::isfinite(policy.maximumSubstepSeconds) || policy.maximumSubsteps == 0U ||
        policy.maximumSpeed < 0.0F || !std::isfinite(policy.maximumSpeed)) {
        return plan;
    }
    plan.enabled = true;
    const float motionLimit = policy.maximumSpeed > 1.0e-8F
        ? policy.maximumDisplacementFraction * policy.characteristicLength / policy.maximumSpeed
        : policy.maximumSubstepSeconds;
    plan.requestedSubstepSeconds = std::min(policy.maximumSubstepSeconds, motionLimit);
    const float safeRequested = std::max(plan.requestedSubstepSeconds, 1.0e-6F);
    const float rawCount = std::ceil(policy.frameDeltaSeconds / safeRequested);
    const std::uint32_t desired = std::max(1U, static_cast<std::uint32_t>(std::min(
        rawCount, static_cast<float>(policy.maximumSubsteps) + 1.0F)));
    plan.substeps = std::min(desired, policy.maximumSubsteps);
    plan.clamped = desired > policy.maximumSubsteps;
    plan.substepSeconds = policy.frameDeltaSeconds / static_cast<float>(plan.substeps);
    plan.estimatedCfl = policy.maximumSpeed * plan.substepSeconds /
                        policy.characteristicLength;
    return plan;
}

} // namespace dve
