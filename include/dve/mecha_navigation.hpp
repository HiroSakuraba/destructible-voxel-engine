#pragma once
// Bounded, deterministic, XY-plane tactical routing for Z-up mechs.
// Independent of the legacy Y-up navigation mesh. No hidden world-wide conversions.
#include "dve/transform.hpp"
#include <cstddef>
#include <functional>
#include <vector>
namespace dve {
struct MechNavigationConfig {
    float cellMeters{3.0F};
    float marginMeters{9.0F};
    std::size_t maximumCells{2304};
};
struct MechNavigationResult {
    std::vector<Float3> waypoints;
    std::size_t examinedCells{};
    bool reachedGoal{};
};
// passable tests the volume occupied by a mech at the supplied world-space XY point.
// The returned path omits the initial position and is bounded by maximumCells.
// A diagonal may not cross a blocked orthogonal corner.
[[nodiscard]] MechNavigationResult plan_mech_navigation_xy(
    Float3 start, Float3 goal, const std::function<bool(Float3)>& passable,
    MechNavigationConfig cfg={});
} // namespace dve
