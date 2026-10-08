#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace dve {
enum class WorldTickPhase : std::uint8_t {
    Clock,
    Timers,
    Destruction,
    Physics,
    Animation,
    RootMotion,
    Gameplay,
    SecondaryMotion,
    Listeners
};
struct WorldTickPhaseSpec {
    WorldTickPhase phase;
    std::string_view name;
    std::uint16_t prerequisites;
};
inline constexpr std::array<WorldTickPhaseSpec, 9> kWorldTickSchedule{
    {{WorldTickPhase::Clock, "clock", 0},
     {WorldTickPhase::Timers, "timers", 1},
     {WorldTickPhase::Destruction, "destruction", 2},
     {WorldTickPhase::Physics, "physics", 4},
     {WorldTickPhase::Animation, "animation", 8},
     {WorldTickPhase::RootMotion, "root motion", 16},
     {WorldTickPhase::Gameplay, "gameplay", 32},
     {WorldTickPhase::SecondaryMotion, "secondary motion", 64},
     {WorldTickPhase::Listeners, "listeners", 128}}};
constexpr bool valid_world_tick_schedule() {
    std::uint16_t completed{};
    for (const auto& spec : kWorldTickSchedule) {
        const auto bit = static_cast<std::uint16_t>(1U << static_cast<unsigned>(spec.phase));
        if ((completed & spec.prerequisites) != spec.prerequisites || (completed & bit))
            return false;
        completed |= bit;
    }
    return completed == 511;
}
static_assert(valid_world_tick_schedule());
struct SystemCadence {
    std::uint32_t every{1}, offset{};
    [[nodiscard]] constexpr bool due(std::uint64_t tick) const noexcept {
        return every && offset < every && tick % every == offset;
    }
};
} // namespace dve
