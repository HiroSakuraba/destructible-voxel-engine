#pragma once
#include "dve/flying_game.hpp"
namespace dve {
struct FlightAeroSample {
    Float3 forceNewton{};
    float airspeed{},angleOfAttack{},liftCoefficient{},dragCoefficient{},dynamicPressure{};
    float stallMarginRadians{};
    bool stalled{};
};
// Pure physical force evaluation: lift is perpendicular to air-relative velocity,
// drag is dissipative, and no powered force is injected by this function.
[[nodiscard]] FlightAeroSample flight_aerodynamics(const FlightProfile&,
    Float3 velocityRelativeToAir,Quaternion orientation,float density=1.225F)noexcept;
} // namespace dve
