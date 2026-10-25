#pragma once
#include "dve/transform.hpp"
#include "dve/water_wake.hpp"
#include "dve/water_coast.hpp"
#include <array>
#include <cstdint>
#include <span>
namespace dve {
// Ocean queries are deterministic pure functions of position and fixed simulation time.
// Z is up, metres/seconds throughout. Renderers and hull probes use this same evaluator.
enum class OceanPreset : std::uint8_t { Calm=0, Breeze=1, Storm=2 };
struct OceanWave {
    Float2 direction{0,1}; // horizontal unit direction
    float amplitude{},wavelength{20},phase{},steepness{0.45F};
};
struct OceanSample {
    float height{};
    Float3 normal{0,0,1};
    Float3 waterVelocity{};
    float foam{}; // dimensionless crest steepness estimate [0,1]
    float depth{500.F},breaking{};
};
class OceanField {
public:
    [[nodiscard]] bool set_preset(OceanPreset preset,Float3 current={}) noexcept;
    [[nodiscard]] bool set_waves(std::span<const OceanWave> waves,Float3 current={}) noexcept;
    [[nodiscard]] OceanSample sample(float x,float y,double seconds)const noexcept;
    [[nodiscard]] OceanPreset preset()const noexcept{return preset_;}
    [[nodiscard]] Float3 current()const noexcept{return current_;}
    [[nodiscard]] std::span<const OceanWave> waves()const noexcept{return {waves_.data(),count_};}
    [[nodiscard]] float maximum_wave_height()const noexcept{return maxHeight_;}
    [[nodiscard]] bool set_coast(const WaterCoast& coast)noexcept{
        if(!valid_water_coast(coast))return false;
        coast_=coast;hasCoast_=true;return true;
    }
    void clear_coast()noexcept{hasCoast_=false;}
    [[nodiscard]] bool has_coast()const noexcept{return hasCoast_;}
    [[nodiscard]] const WaterCoast* coast()const noexcept{return hasCoast_?&coast_:nullptr;}
    [[nodiscard]] bool emit_wake(const WaterWakeEvent& w)noexcept{return wakes_.emit(w);}
    [[nodiscard]] bool restore_wakes(std::span<const WaterWakeEvent> w,double t)noexcept{return wakes_.restore(w,t);}
    void prune_wakes(double t)noexcept{wakes_.prune(t);}
    void clear_wakes()noexcept{wakes_.clear();}
    [[nodiscard]] std::span<const WaterWakeEvent> wakes()const noexcept{return wakes_.events();}
private:
    std::array<OceanWave,16> waves_{};
    std::uint32_t count_{};
    Float3 current_{};
    float maxHeight_{};
    OceanPreset preset_{OceanPreset::Calm};
    WaterWakeField wakes_{};
    WaterCoast coast_{};bool hasCoast_{};
};
[[nodiscard]] bool valid_ocean_wave(const OceanWave&)noexcept;
}
