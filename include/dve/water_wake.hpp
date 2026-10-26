#pragma once
#include "dve/transform.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace dve {
// Analytic local wake impulses. Geometry, buoyancy, and foam read identical data.
struct WaterWakeEvent {
    Float2 position{},direction{0,1};
    double bornSeconds{};
    float amplitude{.25F},wavelength{5},width{2},speed{2.2F},lifetime{6};
};
struct WaterWakeSample {
    float height{},slopeX{},slopeY{},verticalVelocity{},foam{};
};
[[nodiscard]] bool valid_water_wake(const WaterWakeEvent&)noexcept;
class WaterWakeField {
public:
    static constexpr std::size_t MaxEvents=40;
    [[nodiscard]] bool emit(const WaterWakeEvent&)noexcept;
    [[nodiscard]] bool restore(std::span<const WaterWakeEvent>,double now)noexcept;
    void prune(double now)noexcept;
    void clear()noexcept{count_=0;}
    [[nodiscard]] WaterWakeSample sample(float x,float y,double time)const noexcept;
    [[nodiscard]] std::span<const WaterWakeEvent> events()const noexcept{return {events_.data(),count_};}
private:
    std::array<WaterWakeEvent,MaxEvents> events_{};
    std::size_t count_{};
};
}
