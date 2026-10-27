#pragma once
#include <cstdint>
namespace dve {
enum class PressureLevel : std::uint8_t { Normal, Elevated, Recovery, Emergency };
struct WorkBudget {PressureLevel pressure{PressureLevel::Normal};std::uint32_t nearDeadlineMicroseconds{900};std::uint32_t deferredMicroseconds{700};std::uint32_t patchBrickLimit{192};std::uint32_t structuralNodeLimit{4096};bool allowAsyncQualityUpgrades{true};};
}
