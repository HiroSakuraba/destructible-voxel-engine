#pragma once

#include <cstdint>

#include "dve/types.hpp"

namespace dve {

// Solver-neutral contact record. Physics backends may publish these from their contact callback,
// but they must not invoke game audio, allocate memory, or acquire engine locks there.
struct PhysicsContactEvent {
    std::uint64_t bodyA{};
    std::uint64_t bodyB{};
    std::uint16_t materialA{};
    std::uint16_t materialB{};
    Float3 position{};
    Float3 relativeVelocity{};
    Float3 contactNormal{0.0F, 1.0F, 0.0F};
    float normalImpulse{};
    float effectiveMass{};
    bool persistent{};
    double timeSeconds{};
};

class IPhysicsContactSink {
public:
    virtual ~IPhysicsContactSink() = default;
    virtual bool record_contact(const PhysicsContactEvent& event) noexcept = 0;
    virtual void body_removed(std::uint64_t body) noexcept { (void)body; }
};

} // namespace dve
