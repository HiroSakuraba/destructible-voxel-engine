#pragma once
#include "dve/transform.hpp"
namespace dve {
// A small, local shoaling/breaking surf-zone approximation, not a full shallow-water solver.
// Offshore is -Y; the beach lies at shoreY and coast wave crests propagate toward +Y.
// Exposed independently of graphics for deterministic physics, editing and headless tests.
struct WaterCoast {
    Float2 shoreline{0,0};  // x center, y waterline in world metres
    float halfWidth{150},offshoreLength{240};
    float slope{.05F};     // seabed rise per horizontal metre
    float swellHeight{1.4F},periodSeconds{9},peel{.08F};
};
struct WaterCoastSample {
    float depth{500},height{},slopeX{},slopeY{},verticalVelocity{},flowY{};
    float breaking{},foam{};
    bool inZone{};
};
[[nodiscard]] bool valid_water_coast(const WaterCoast&)noexcept;
[[nodiscard]] WaterCoastSample sample_water_coast(
    const WaterCoast&,float worldX,float worldY,double seconds)noexcept;
}
