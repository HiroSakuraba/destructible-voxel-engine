#pragma once
#include "dve/transform.hpp"
#include <cstdint>
#include <vector>
namespace dve {
// World-space wind, metres/second. No force is directly applied to the aircraft:
// flight aerodynamics use velocity of the vehicle relative to moving air.
enum class FlightWindKind : std::uint8_t { Thermal, Ridge };
struct FlightWindZone {
    std::uint64_t stableId{};
    FlightWindKind kind{FlightWindKind::Thermal};
    RigidTransform transform{};
    float radiusMeters{80.0F};
    float heightMeters{260.0F};
    float strengthMetersPerSecond{6.0F};
    float sinkFraction{0.25F};
};
struct FlightWindSample {
    Float3 velocity{};
    float thermalVertical{};
    float ridgeVertical{};
    float backgroundVertical{};
};
class FlightWindField {
public:
    // Horizontal background and shear in m/s. Gusts are analytic and deterministic.
    void configure(Float3 horizontalWind,float shearPerHundredMeters=0.0F,
                   float gustStrength=0.0F) noexcept;
    void set_zones(std::vector<FlightWindZone> zones);
    [[nodiscard]] const std::vector<FlightWindZone>& zones()const noexcept{return zones_;}
    [[nodiscard]] FlightWindSample sample(Float3 position,float elapsedSeconds)const noexcept;
private:
    Float3 base_{};
    float shear_{},gust_{};
    std::vector<FlightWindZone> zones_;
};
[[nodiscard]] bool validate_flight_wind_zone(const FlightWindZone&)noexcept;
} // namespace dve
