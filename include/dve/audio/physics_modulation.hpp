// Physics-based modulation sources: springs, pendulums, orbiters, and
// attractors as alternatives to LFOs. All are realtime-safe (no allocation).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dve::audio {

// A damped spring (mass on a spring). Excited by impulses, oscillates and decays.
// Output: position in -1..1 (normalized by excitation).
struct SpringModulator {
    float position{0.0F};
    float velocity{0.0F};
    float stiffness{40.0F};  // radians/sec^2
    float damping{2.0F};     // 1/sec

    void excite(float impulse) noexcept { velocity += impulse; }
    float step(float dt) noexcept;
    void reset() noexcept { position = 0.0F; velocity = 0.0F; }
};

// A nonlinear pendulum. Can swing, orbit, or go chaotic with enough energy.
// Output: sin(angle) in -1..1.
struct PendulumModulator {
    float angle{0.1F};
    float angularVelocity{0.0F};
    float length{1.0F};   // meters (affects frequency)
    float damping{0.5F};
    float gravity{9.81F};

    void excite(float impulse) noexcept { angularVelocity += impulse; }
    float step(float dt) noexcept;
    void reset() noexcept { angle = 0.1F; angularVelocity = 0.0F; }
};

// A 2D orbiter: a particle in a central force field (elliptical orbits).
// Output: x position normalized to -1..1.
struct OrbiterModulator {
    float x{1.0F}, y{0.0F};
    float vx{0.0F}, vy{0.8F};
    float centralMass{2.0F};
    float damping{0.05F};

    void excite(float ix, float iy) noexcept { vx += ix; vy += iy; }
    float step(float dt) noexcept;
    void reset() noexcept { x = 1.0F; y = 0.0F; vx = 0.0F; vy = 0.8F; }
};

// Lorenz attractor (chaotic). Output: normalized x in -1..1.
// Never repeats — organic, evolving modulation.
struct LorenzModulator {
    float x{0.1F}, y{0.0F}, z{0.0F};
    float sigma{10.0F}, rho{28.0F}, beta{2.6666667F};
    float speed{1.0F}; // time scale

    float step(float dt) noexcept;
    void reset() noexcept { x = 0.1F; y = 0.0F; z = 0.0F; }
};

// Combined physics modulation bank (one per synth, global).
struct PhysicsModulationBank {
    SpringModulator spring;
    PendulumModulator pendulum;
    OrbiterModulator orbiter;
    LorenzModulator lorenz;

    // Step all modulators. Call once per render block.
    void step(float dt) noexcept;
    // Excite from a note event (velocity 0..1).
    void note_on(float velocity) noexcept;
    void reset() noexcept;
};

} // namespace dve::audio
